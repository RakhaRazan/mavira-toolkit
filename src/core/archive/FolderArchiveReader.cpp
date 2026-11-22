#include "core/archive/FolderArchiveReader.hpp"

#include "data/fs/FileManager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

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

[[nodiscard]] Result<std::vector<std::uint8_t>> readFileInChunks(
    const std::filesystem::path& filePath,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::Cancelled, "Page read cancelled");
    }

    const std::filesystem::path longPath(FileManager::toWin32LongPath(filePath));
    std::ifstream ifs(longPath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::IoError,
            "Failed to open image file: " + FileManager::wideToUtf8(filePath.native()));
    }

    const std::streampos endPos = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> buffer;
    if (endPos > 0) {
        buffer.reserve(static_cast<std::size_t>(endPos));
    }

    std::uint8_t chunk[kReadChunkBytes];
    while (ifs) {
        if (stopToken.stop_requested()) {
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::Cancelled, "Page read cancelled");
        }

        ifs.read(reinterpret_cast<char*>(chunk), static_cast<std::streamsize>(sizeof(chunk)));
        const std::streamsize bytesRead = ifs.gcount();
        if (bytesRead > 0) {
            buffer.insert(buffer.end(), chunk, chunk + static_cast<std::size_t>(bytesRead));
        }
        if (ifs.eof()) {
            break;
        }
        if (ifs.fail()) {
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::IoError,
                "Failed while reading image file: " + FileManager::wideToUtf8(filePath.native()));
        }
    }

    return Result<std::vector<std::uint8_t>>::ok(std::move(buffer));
}

} // namespace

Result<void> FolderArchiveReader::open(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);

    isOpen_ = false;
    pages_.clear();
    metadataPath_.reset();

    if (path.empty()) {
        return Result<void>::err(ErrorCode::InvalidArgument, "Folder path is empty");
    }

    const std::filesystem::path longPath(FileManager::toWin32LongPath(path));
    std::error_code ec;
    if (!std::filesystem::exists(longPath, ec) || ec) {
        return Result<void>::err(
            ErrorCode::NotFound,
            "Folder does not exist: " + FileManager::wideToUtf8(path.native()));
    }

    if (!std::filesystem::is_directory(longPath, ec) || ec) {
        return Result<void>::err(
            ErrorCode::InvalidArgument,
            "Path is not a directory: " + FileManager::wideToUtf8(path.native()));
    }

    std::vector<PageEntry> indexedPages;
    std::optional<std::filesystem::path> foundMetadataPath;

    const auto iterOptions = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator it(longPath, iterOptions, ec);
    if (ec) {
        return Result<void>::err(
            ErrorCode::IoError,
            "Failed to iterate folder: " + ec.message());
    }

    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto& entry = *it;
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            continue;
        }

        const std::filesystem::path entryPath = entry.path();
        const std::string fileNameUtf8 = FileManager::wideToUtf8(entryPath.filename().native());
        if (fileNameUtf8.empty() || fileNameUtf8.starts_with("._")) {
            continue;
        }

        if (toLowerAscii(fileNameUtf8) == "comicinfo.xml") {
            if (!foundMetadataPath.has_value()) {
                foundMetadataPath = entryPath;
            }
            continue;
        }

        if (FileManager::isSupportedImageExtension(entryPath)) {
            std::error_code szEc;
            const auto fsize = entry.file_size(szEc);
            if (!szEc && fsize > 0) {
                indexedPages.push_back(PageEntry{entryPath, fileNameUtf8});
            }
        }
    }

    std::sort(
        indexedPages.begin(),
        indexedPages.end(),
        [](const PageEntry& lhs, const PageEntry& rhs) {
            return FileManager::naturalLess(lhs.name, rhs.name);
        });

    pages_ = std::move(indexedPages);
    metadataPath_ = std::move(foundMetadataPath);
    isOpen_ = true;

    return Result<void>::ok();
}

void FolderArchiveReader::close() noexcept {
    std::scoped_lock lock(mutex_);
    isOpen_ = false;
    pages_.clear();
    metadataPath_.reset();
}

bool FolderArchiveReader::isOpen() const noexcept {
    std::scoped_lock lock(mutex_);
    return isOpen_;
}

std::size_t FolderArchiveReader::pageCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return pages_.size();
}

std::string FolderArchiveReader::pageName(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    if (index >= pages_.size()) {
        return {};
    }
    return pages_[index].name;
}

Result<std::vector<std::uint8_t>> FolderArchiveReader::readPageBytes(
    std::size_t index,
    std::stop_token stopToken) {
    std::filesystem::path targetPath;
    {
        std::scoped_lock lock(mutex_);
        if (!isOpen_) {
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::InvalidArgument, "Folder reader is not open");
        }
        if (index >= pages_.size()) {
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::InvalidArgument, "Page index out of range");
        }
        targetPath = pages_[index].filePath;
    }

    return readFileInChunks(targetPath, stopToken);
}

Result<std::optional<std::string>> FolderArchiveReader::readMetadataXml() {
    std::optional<std::filesystem::path> metaPath;
    {
        std::scoped_lock lock(mutex_);
        if (!isOpen_) {
            return Result<std::optional<std::string>>::err(
                ErrorCode::InvalidArgument, "Folder reader is not open");
        }
        metaPath = metadataPath_;
    }

    if (!metaPath.has_value()) {
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    auto bytesRes = readFileInChunks(*metaPath, {});
    if (!bytesRes.isOk()) {
        return Result<std::optional<std::string>>::err(bytesRes.error());
    }

    const auto& bytes = bytesRes.value();
    std::string xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return Result<std::optional<std::string>>::ok(std::move(xml));
}

} // namespace mavira::core::archive
