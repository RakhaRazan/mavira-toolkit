#include "core/archive/ZipArchiveReader.hpp"

#include "data/fs/FileManager.hpp"

#include <zip.h>

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

[[nodiscard]] bool isIgnoredArchiveEntry(std::string_view entryName) noexcept {
    if (entryName.empty()) {
        return true;
    }
    // Skip directory entries.
    if (entryName.back() == '/' || entryName.back() == '\\') {
        return true;
    }
    // Skip macOS resource fork metadata folders and AppleDouble files.
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

[[nodiscard]] ErrorCode mapZipErrorCode(int zipErrCode) noexcept {
    switch (zipErrCode) {
        case ZIP_ER_NOENT:
            return ErrorCode::NotFound;
        case ZIP_ER_NOZIP:
        case ZIP_ER_INCONS:
        case ZIP_ER_CRC:
        case ZIP_ER_ZLIB:
        case ZIP_ER_EOF:
        case ZIP_ER_COMPNOTSUPP:
        case ZIP_ER_ENCRNOTSUPP:
            return ErrorCode::CorruptArchive;
        case ZIP_ER_MEMORY:
            return ErrorCode::OutOfMemory;
        case ZIP_ER_INVAL:
            return ErrorCode::InvalidArgument;
        case ZIP_ER_OPEN:
        case ZIP_ER_READ:
        case ZIP_ER_SEEK:
        default:
            return ErrorCode::IoError;
    }
}

/// Opens a libzip archive using a wide UTF-16 Win32 path (`zip_source_win32w_create` +
/// `zip_open_from_source`) so Unicode and extended-length (`\\?\`) paths work natively.
[[nodiscard]] zip* openZipWin32Wide(const std::wstring& widePath, zip_error_t& zipErr) noexcept {
    zip_source_t* source = ::zip_source_win32w_create(widePath.c_str(), 0, -1, &zipErr);
    if (source == nullptr) {
        return nullptr;
    }

    zip_t* archive = ::zip_open_from_source(source, ZIP_RDONLY, &zipErr);
    if (archive == nullptr) {
        // Ownership transfers to `archive` only on success; free on failure.
        ::zip_source_free(source);
        return nullptr;
    }
    return archive;
}

} // namespace

ZipArchiveReader::~ZipArchiveReader() {
    close();
}

Result<void> ZipArchiveReader::open(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);

    if (archive_ != nullptr) {
        ::zip_discard(archive_);
        archive_ = nullptr;
    }
    pages_.clear();
    metadataIndex_.reset();

    if (path.empty()) {
        return Result<void>::err(ErrorCode::InvalidArgument, "ZIP archive path is empty");
    }

    const std::wstring widePath = FileManager::toWin32LongPath(path);

    zip_error_t zipErr;
    ::zip_error_init(&zipErr);

    zip_t* archive = openZipWin32Wide(widePath, zipErr);
    if (archive == nullptr) {
        const ErrorCode errCode = mapZipErrorCode(::zip_error_code_zip(&zipErr));
        std::string message = "Failed to open ZIP archive: ";
        message += ::zip_error_strerror(&zipErr);
        ::zip_error_fini(&zipErr);
        return Result<void>::err(errCode, std::move(message));
    }
    ::zip_error_fini(&zipErr);

    // O(1) central directory indexing: libzip has already parsed the central directory into RAM.
    const zip_int64_t numEntries = ::zip_get_num_entries(archive, 0);
    if (numEntries < 0) {
        ::zip_discard(archive);
        return Result<void>::err(ErrorCode::CorruptArchive, "Failed to read ZIP entry count");
    }

    std::vector<PageEntry> indexedPages;
    indexedPages.reserve(static_cast<std::size_t>(numEntries));

    std::optional<std::uint64_t> foundMetadataIndex;
    bool metadataIsRoot = false;

    for (zip_int64_t i = 0; i < numEntries; ++i) {
        const auto zipIdx = static_cast<zip_uint64_t>(i);
        const char* rawName = ::zip_get_name(archive, zipIdx, ZIP_FL_ENC_GUESS);
        if (rawName == nullptr) {
            continue;
        }

        const std::string_view entryView(rawName);
        if (isIgnoredArchiveEntry(entryView)) {
            continue;
        }

        if (isComicInfoXml(entryView)) {
            const bool isRoot = isRootLevelEntry(entryView);
            if (!foundMetadataIndex.has_value() || (isRoot && !metadataIsRoot)) {
                foundMetadataIndex = static_cast<std::uint64_t>(zipIdx);
                metadataIsRoot = isRoot;
            }
            continue;
        }

        const std::filesystem::path entryFsPath(FileManager::utf8ToWide(entryView));
        if (FileManager::isSupportedImageExtension(entryFsPath)) {
            zip_stat_t st;
            ::zip_stat_init(&st);
            if (::zip_stat_index(archive, zipIdx, 0, &st) == 0 &&
                (st.valid & ZIP_STAT_SIZE) != 0 &&
                st.size == 0) {
                continue;
            }
            indexedPages.push_back(PageEntry{
                static_cast<std::uint64_t>(zipIdx),
                std::string(entryView)
            });
        }
    }

    std::sort(
        indexedPages.begin(),
        indexedPages.end(),
        [](const PageEntry& lhs, const PageEntry& rhs) {
            const int cmp = FileManager::naturalCompare(lhs.name, rhs.name);
            if (cmp != 0) {
                return cmp < 0;
            }
            return lhs.zipIndex < rhs.zipIndex;
        });

    archive_ = archive;
    pages_ = std::move(indexedPages);
    metadataIndex_ = foundMetadataIndex;

    return Result<void>::ok();
}

void ZipArchiveReader::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (archive_ != nullptr) {
        ::zip_discard(archive_);
        archive_ = nullptr;
    }
    pages_.clear();
    metadataIndex_.reset();
}

bool ZipArchiveReader::isOpen() const noexcept {
    std::scoped_lock lock(mutex_);
    return archive_ != nullptr;
}

std::size_t ZipArchiveReader::pageCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return pages_.size();
}

std::string ZipArchiveReader::pageName(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    if (index >= pages_.size()) {
        return {};
    }
    return pages_[index].name;
}

Result<std::vector<std::uint8_t>> ZipArchiveReader::readEntryBytesLocked(
    std::uint64_t zipIndex,
    std::stop_token stopToken) {
    if (archive_ == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "ZIP archive is not open");
    }

    if (stopToken.stop_requested()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::Cancelled, "Page read cancelled");
    }

    zip_stat_t st;
    ::zip_stat_init(&st);
    const bool hasStat = (::zip_stat_index(archive_, zipIndex, 0, &st) == 0);

    zip_file_t* zf = ::zip_fopen_index(archive_, zipIndex, 0);
    if (zf == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::CorruptArchive,
            std::string("Failed to open ZIP entry: ") + ::zip_strerror(archive_));
    }

    std::vector<std::uint8_t> buffer;
    if (hasStat && (st.valid & ZIP_STAT_SIZE) != 0) {
        buffer.reserve(static_cast<std::size_t>(st.size));
    }

    std::uint8_t chunk[kReadChunkBytes];
    for (;;) {
        if (stopToken.stop_requested()) {
            ::zip_fclose(zf);
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::Cancelled, "Page read cancelled");
        }

        const zip_int64_t bytesRead = ::zip_fread(zf, chunk, sizeof(chunk));
        if (bytesRead < 0) {
            std::string msg = "Failed to read ZIP entry: ";
            msg += ::zip_file_strerror(zf);
            ::zip_fclose(zf);
            return Result<std::vector<std::uint8_t>>::err(ErrorCode::CorruptArchive, std::move(msg));
        }
        if (bytesRead == 0) {
            break;
        }

        buffer.insert(buffer.end(), chunk, chunk + static_cast<std::size_t>(bytesRead));
    }

    if (::zip_fclose(zf) != 0) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::CorruptArchive,
            std::string("Failed to close ZIP entry: ") + ::zip_strerror(archive_));
    }

    return Result<std::vector<std::uint8_t>>::ok(std::move(buffer));
}

Result<std::vector<std::uint8_t>> ZipArchiveReader::readPageBytes(
    std::size_t index,
    std::stop_token stopToken) {
    std::scoped_lock lock(mutex_);
    if (archive_ == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "ZIP archive is not open");
    }
    if (index >= pages_.size()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Page index out of range");
    }

    return readEntryBytesLocked(pages_[index].zipIndex, stopToken);
}

Result<std::optional<std::string>> ZipArchiveReader::readMetadataXml() {
    std::scoped_lock lock(mutex_);
    if (archive_ == nullptr) {
        return Result<std::optional<std::string>>::err(
            ErrorCode::InvalidArgument, "ZIP archive is not open");
    }
    if (!metadataIndex_.has_value()) {
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    auto bytesRes = readEntryBytesLocked(*metadataIndex_, {});
    if (!bytesRes.isOk()) {
        return Result<std::optional<std::string>>::err(bytesRes.error());
    }

    const auto& bytes = bytesRes.value();
    std::string xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return Result<std::optional<std::string>>::ok(std::move(xml));
}

} // namespace mavira::core::archive
