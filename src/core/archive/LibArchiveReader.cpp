#include "core/archive/LibArchiveReader.hpp"

#include "data/fs/FileManager.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mavira::core::archive {

namespace {

using mavira::core::ErrorCode;
using mavira::core::Result;
using mavira::data::fs::FileManager;

constexpr std::size_t kArchiveBlockSize = 64 * 1024;
constexpr std::size_t kReadChunkBytes = 64 * 1024;

[[nodiscard]] std::string toLowerAscii(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] std::string_view extractFilename(std::string_view entryPath) noexcept {
    const std::size_t pos = entryPath.find_last_of("/\\");
    return (pos == std::string_view::npos) ? entryPath : entryPath.substr(pos + 1);
}

[[nodiscard]] bool isIgnoredEntry(std::string_view entryName) noexcept {
    if (entryName.empty()) {
        return true;
    }
    if (entryName.back() == '/' || entryName.back() == '\\') {
        return true;
    }
    if (entryName.starts_with("__MACOSX/") ||
        entryName.find("/__MACOSX/") != std::string_view::npos) {
        return true;
    }
    const std::string_view filename = extractFilename(entryName);
    if (filename.empty() || filename.starts_with("._")) {
        return true;
    }
    return false;
}

[[nodiscard]] bool isComicInfoXml(std::string_view entryName) {
    return toLowerAscii(extractFilename(entryName)) == "comicinfo.xml";
}

[[nodiscard]] bool isRootLevelEntry(std::string_view entryName) noexcept {
    return entryName.find('/') == std::string_view::npos &&
           entryName.find('\\') == std::string_view::npos;
}

[[nodiscard]] std::string extractEntryNameUtf8(archive_entry* entry) {
    if (const wchar_t* wideName = ::archive_entry_pathname_w(entry); wideName != nullptr) {
        return FileManager::wideToUtf8(wideName);
    }
    if (const char* utf8Name = ::archive_entry_pathname_utf8(entry); utf8Name != nullptr) {
        return std::string(utf8Name);
    }
    if (const char* rawName = ::archive_entry_pathname(entry); rawName != nullptr) {
        return std::string(rawName);
    }
    return {};
}

[[nodiscard]] struct ::archive* createConfiguredArchive() {
    struct ::archive* a = ::archive_read_new();
    if (a == nullptr) {
        return nullptr;
    }
    ::archive_read_support_filter_all(a);
    ::archive_read_support_format_rar(a);
    ::archive_read_support_format_rar5(a);
    ::archive_read_support_format_7zip(a);
    ::archive_read_support_format_tar(a);
    return a;
}

} // namespace

LibArchiveReader::~LibArchiveReader() {
    close();
}

void LibArchiveReader::closeStreamLocked() noexcept {
    if (stream_ != nullptr) {
        ::archive_read_free(stream_);
        stream_ = nullptr;
    }
    nextArchiveIndex_ = 0;
}

Result<void> LibArchiveReader::reopenStreamLocked() {
    closeStreamLocked();

    struct ::archive* a = createConfiguredArchive();
    if (a == nullptr) {
        return Result<void>::err(ErrorCode::OutOfMemory, "Failed to allocate libarchive handle");
    }

    const int rc = ::archive_read_open_filename_w(a, wideLongPath_.c_str(), kArchiveBlockSize);
    if (rc != ARCHIVE_OK) {
        std::string msg = "Failed to open archive via libarchive: ";
        if (const char* errStr = ::archive_error_string(a); errStr != nullptr) {
            msg += errStr;
        }
        ::archive_read_free(a);
        return Result<void>::err(ErrorCode::IoError, std::move(msg));
    }

    stream_ = a;
    nextArchiveIndex_ = 0;
    return Result<void>::ok();
}

Result<void> LibArchiveReader::open(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);

    closeStreamLocked();
    isOpen_ = false;
    wideLongPath_.clear();
    pages_.clear();
    metadataArchiveIndex_.reset();

    if (path.empty()) {
        return Result<void>::err(ErrorCode::InvalidArgument, "Archive path is empty");
    }

    if (!FileManager::isPathOnline(path)) {
        return Result<void>::err(
            ErrorCode::NotFound,
            "Archive file not found: " + FileManager::wideToUtf8(path.native()));
    }

    wideLongPath_ = FileManager::toWin32LongPath(path);

    auto openRes = reopenStreamLocked();
    if (!openRes.isOk()) {
        wideLongPath_.clear();
        return openRes;
    }

    std::vector<PageEntry> indexedPages;
    std::optional<std::size_t> foundMetadataIndex;
    bool metadataIsRoot = false;

    archive_entry* entry = nullptr;
    std::size_t entryIdx = 0;
    for (;;) {
        const int rc = ::archive_read_next_header(stream_, &entry);
        if (rc == ARCHIVE_EOF) {
            break;
        }
        if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
            std::string msg = "Failed to read archive header: ";
            if (const char* errStr = ::archive_error_string(stream_); errStr != nullptr) {
                msg += errStr;
            }
            closeStreamLocked();
            wideLongPath_.clear();
            return Result<void>::err(ErrorCode::CorruptArchive, std::move(msg));
        }

        const bool isRegularFile = (::archive_entry_filetype(entry) == AE_IFREG);
        if (isRegularFile) {
            const std::string entryName = extractEntryNameUtf8(entry);
            if (!isIgnoredEntry(entryName)) {
                if (isComicInfoXml(entryName)) {
                    const bool isRoot = isRootLevelEntry(entryName);
                    if (!foundMetadataIndex.has_value() || (isRoot && !metadataIsRoot)) {
                        foundMetadataIndex = entryIdx;
                        metadataIsRoot = isRoot;
                    }
                } else {
                    const std::filesystem::path entryFsPath(FileManager::utf8ToWide(entryName));
                    if (FileManager::isSupportedImageExtension(entryFsPath)) {
                        const bool isEmptyEntry =
                            ::archive_entry_size_is_set(entry) && ::archive_entry_size(entry) <= 0;
                        if (!isEmptyEntry) {
                            indexedPages.push_back(PageEntry{entryIdx, entryName});
                        }
                    }
                }
            }
        }

        ::archive_read_data_skip(stream_);
        ++entryIdx;
    }

    // Close the indexing stream; a fresh streaming cursor will open lazily on first read.
    closeStreamLocked();

    std::sort(
        indexedPages.begin(),
        indexedPages.end(),
        [](const PageEntry& lhs, const PageEntry& rhs) {
            const int cmp = FileManager::naturalCompare(lhs.name, rhs.name);
            if (cmp != 0) {
                return cmp < 0;
            }
            return lhs.archiveIndex < rhs.archiveIndex;
        });

    pages_ = std::move(indexedPages);
    metadataArchiveIndex_ = foundMetadataIndex;
    isOpen_ = true;

    return Result<void>::ok();
}

void LibArchiveReader::close() noexcept {
    std::scoped_lock lock(mutex_);
    closeStreamLocked();
    isOpen_ = false;
    wideLongPath_.clear();
    pages_.clear();
    metadataArchiveIndex_.reset();
}

bool LibArchiveReader::isOpen() const noexcept {
    std::scoped_lock lock(mutex_);
    return isOpen_;
}

std::size_t LibArchiveReader::pageCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return pages_.size();
}

std::string LibArchiveReader::pageName(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    if (index >= pages_.size()) {
        return {};
    }
    return pages_[index].name;
}

Result<std::vector<std::uint8_t>> LibArchiveReader::readEntryByArchiveIndexLocked(
    std::size_t targetArchiveIndex,
    std::stop_token stopToken) {
    if (!isOpen_) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Archive is not open");
    }

    if (stopToken.stop_requested()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::Cancelled, "Archive read cancelled");
    }

    // Reopen stream if no cursor is active or if seeking backwards in the archive stream.
    if (stream_ == nullptr || targetArchiveIndex < nextArchiveIndex_) {
        auto reopenRes = reopenStreamLocked();
        if (!reopenRes.isOk()) {
            return Result<std::vector<std::uint8_t>>::err(reopenRes.error());
        }
    }

    archive_entry* entry = nullptr;
    while (nextArchiveIndex_ <= targetArchiveIndex) {
        if (stopToken.stop_requested()) {
            closeStreamLocked();
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::Cancelled, "Archive read cancelled");
        }

        const int rc = ::archive_read_next_header(stream_, &entry);
        if (rc == ARCHIVE_EOF) {
            closeStreamLocked();
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::CorruptArchive, "Unexpected EOF while seeking archive entry");
        }
        if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
            std::string msg = "Failed to seek archive entry: ";
            if (const char* errStr = ::archive_error_string(stream_); errStr != nullptr) {
                msg += errStr;
            }
            closeStreamLocked();
            return Result<std::vector<std::uint8_t>>::err(ErrorCode::CorruptArchive, std::move(msg));
        }

        const std::size_t currentIndex = nextArchiveIndex_++;
        if (currentIndex < targetArchiveIndex) {
            ::archive_read_data_skip(stream_);
            continue;
        }

        // Found target entry: stream directly into RAM without temp files.
        std::vector<std::uint8_t> buffer;
        if (::archive_entry_size_is_set(entry)) {
            const la_int64_t expectedSize = ::archive_entry_size(entry);
            if (expectedSize > 0) {
                buffer.reserve(static_cast<std::size_t>(expectedSize));
            }
        }

        std::uint8_t chunk[kReadChunkBytes];
        for (;;) {
            if (stopToken.stop_requested()) {
                closeStreamLocked();
                return Result<std::vector<std::uint8_t>>::err(
                    ErrorCode::Cancelled, "Archive read cancelled");
            }

            const la_ssize_t bytesRead = ::archive_read_data(stream_, chunk, sizeof(chunk));
            if (bytesRead < 0) {
                std::string msg = "Failed to read archive entry data: ";
                if (const char* errStr = ::archive_error_string(stream_); errStr != nullptr) {
                    msg += errStr;
                }
                closeStreamLocked();
                return Result<std::vector<std::uint8_t>>::err(
                    ErrorCode::CorruptArchive, std::move(msg));
            }
            if (bytesRead == 0) {
                break;
            }
            buffer.insert(buffer.end(), chunk, chunk + static_cast<std::size_t>(bytesRead));
        }

        return Result<std::vector<std::uint8_t>>::ok(std::move(buffer));
    }

    return Result<std::vector<std::uint8_t>>::err(
        ErrorCode::NotFound, "Target archive entry index not found");
}

Result<std::vector<std::uint8_t>> LibArchiveReader::readPageBytes(
    std::size_t index,
    std::stop_token stopToken) {
    std::scoped_lock lock(mutex_);
    if (!isOpen_) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Archive is not open");
    }
    if (index >= pages_.size()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Page index out of range");
    }

    return readEntryByArchiveIndexLocked(pages_[index].archiveIndex, stopToken);
}

Result<std::optional<std::string>> LibArchiveReader::readMetadataXml() {
    std::scoped_lock lock(mutex_);
    if (!isOpen_) {
        return Result<std::optional<std::string>>::err(
            ErrorCode::InvalidArgument, "Archive is not open");
    }
    if (!metadataArchiveIndex_.has_value()) {
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    auto bytesRes = readEntryByArchiveIndexLocked(*metadataArchiveIndex_, {});
    if (!bytesRes.isOk()) {
        return Result<std::optional<std::string>>::err(bytesRes.error());
    }

    const auto& bytes = bytesRes.value();
    std::string xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return Result<std::optional<std::string>>::ok(std::move(xml));
}

} // namespace mavira::core::archive
