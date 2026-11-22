#include "core/archive/ArchiveEngine.hpp"

#include "core/archive/ComicMerger.hpp"
#include "core/archive/FolderArchiveReader.hpp"
#include "core/archive/LibArchiveReader.hpp"
#include "core/archive/PdfDocumentReader.hpp"
#include "core/archive/ZipArchiveReader.hpp"
#include "data/fs/FileManager.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace mavira::core::archive {

namespace {

using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::Result;
using mavira::data::fs::FileManager;

constexpr std::size_t kHeaderSniffBytes = 512;

[[nodiscard]] bool startsWith(
    const std::array<std::uint8_t, kHeaderSniffBytes>& buf,
    std::size_t size,
    const std::uint8_t* sig,
    std::size_t sigLen) noexcept {
    if (size < sigLen) {
        return false;
    }
    return std::memcmp(buf.data(), sig, sigLen) == 0;
}

[[nodiscard]] ComicFormat detectFormatFromMagicBytes(
    const std::filesystem::path& path,
    ComicFormat extFormat) {
    const std::filesystem::path longPath(FileManager::toWin32LongPath(path));
    std::ifstream ifs(longPath, std::ios::binary);
    if (!ifs.is_open()) {
        return ComicFormat::Unknown;
    }

    std::array<std::uint8_t, kHeaderSniffBytes> header{};
    ifs.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const std::size_t bytesRead = static_cast<std::size_t>(ifs.gcount());
    if (bytesRead < 4) {
        return ComicFormat::Unknown;
    }

    // ZIP / CBZ / EPUB: PK\x03\x04 (local file header), PK\x05\x06 (empty EOCD), PK\x07\x08 (spanned)
    static constexpr std::uint8_t kZipLocal[4] = {0x50, 0x4B, 0x03, 0x04};
    static constexpr std::uint8_t kZipEmpty[4] = {0x50, 0x4B, 0x05, 0x06};
    static constexpr std::uint8_t kZipSpanned[4] = {0x50, 0x4B, 0x07, 0x08};
    if (startsWith(header, bytesRead, kZipLocal, sizeof(kZipLocal)) ||
        startsWith(header, bytesRead, kZipEmpty, sizeof(kZipEmpty)) ||
        startsWith(header, bytesRead, kZipSpanned, sizeof(kZipSpanned))) {
        // EPUB spec requires an uncompressed "mimetype" entry with "application/epub+zip"
        // starting at offset 30 (immediately after the 30-byte local file header).
        static constexpr char kEpubMarker[] = "mimetypeapplication/epub+zip";
        constexpr std::size_t kEpubOffset = 30;
        constexpr std::size_t kEpubMarkerLen = sizeof(kEpubMarker) - 1;
        if (extFormat == ComicFormat::EPUB ||
            (bytesRead >= kEpubOffset + kEpubMarkerLen &&
             std::memcmp(header.data() + kEpubOffset, kEpubMarker, kEpubMarkerLen) == 0)) {
            return ComicFormat::EPUB;
        }
        return ComicFormat::CBZ;
    }

    // PDF: "%PDF-"
    static constexpr std::uint8_t kPdfMagic[5] = {0x25, 0x50, 0x44, 0x46, 0x2D};
    if (startsWith(header, bytesRead, kPdfMagic, sizeof(kPdfMagic))) {
        return ComicFormat::PDF;
    }

    // RAR4: "Rar!\x1A\x07\x00" and RAR5: "Rar!\x1A\x07\x01\x00"
    static constexpr std::uint8_t kRar4Magic[7] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
    static constexpr std::uint8_t kRar5Magic[8] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00};
    if (startsWith(header, bytesRead, kRar4Magic, sizeof(kRar4Magic)) ||
        startsWith(header, bytesRead, kRar5Magic, sizeof(kRar5Magic))) {
        return ComicFormat::CBR;
    }

    // 7-Zip: "7z\xBC\xAF\x27\x1C"
    static constexpr std::uint8_t k7zMagic[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
    if (startsWith(header, bytesRead, k7zMagic, sizeof(k7zMagic))) {
        return ComicFormat::CB7;
    }

    // POSIX TAR: "ustar" at byte offset 257
    static constexpr char kTarMagic[] = "ustar";
    constexpr std::size_t kTarOffset = 257;
    if (bytesRead >= kTarOffset + 5 &&
        std::memcmp(header.data() + kTarOffset, kTarMagic, 5) == 0) {
        return ComicFormat::CBT;
    }

    return ComicFormat::Unknown;
}

} // namespace

ComicFormat ArchiveEngine::detectFormat(const std::filesystem::path& path) {
    if (path.empty()) {
        return ComicFormat::Unknown;
    }

    const std::filesystem::path longPath(FileManager::toWin32LongPath(path));
    std::error_code ec;
    if (std::filesystem::is_directory(longPath, ec) && !ec) {
        return ComicFormat::Folder;
    }

    const ComicFormat extFormat = FileManager::detectFormatFromExtension(path);
    const ComicFormat magicFormat = detectFormatFromMagicBytes(path, extFormat);
    if (magicFormat != ComicFormat::Unknown) {
        return magicFormat;
    }

    return extFormat;
}

Result<std::unique_ptr<IArchiveReader>> ArchiveEngine::createReader(ComicFormat format) {
    switch (format) {
        case ComicFormat::CBZ:
        case ComicFormat::EPUB:
            return Result<std::unique_ptr<IArchiveReader>>::ok(
                std::make_unique<ZipArchiveReader>());

        case ComicFormat::Folder:
            return Result<std::unique_ptr<IArchiveReader>>::ok(
                std::make_unique<FolderArchiveReader>());

        case ComicFormat::CBR:
        case ComicFormat::CB7:
        case ComicFormat::CBT:
            return Result<std::unique_ptr<IArchiveReader>>::ok(
                std::make_unique<LibArchiveReader>());

        case ComicFormat::PDF:
            return Result<std::unique_ptr<IArchiveReader>>::ok(
                std::make_unique<PdfDocumentReader>());

        case ComicFormat::Unknown:
        default:
            return Result<std::unique_ptr<IArchiveReader>>::err(
                ErrorCode::UnsupportedFormat,
                "Unsupported or unrecognized archive format");
    }
}

Result<std::unique_ptr<IArchiveReader>> ArchiveEngine::openArchive(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return Result<std::unique_ptr<IArchiveReader>>::err(
            ErrorCode::InvalidArgument,
            "Archive path is empty");
    }

    if (!FileManager::isPathOnline(path)) {
        const ErrorCode code = FileManager::isVolumeOnline(path)
            ? ErrorCode::NotFound
            : ErrorCode::OfflineVolume;
        return Result<std::unique_ptr<IArchiveReader>>::err(
            code,
            "Archive path is unavailable: " + FileManager::wideToUtf8(path.native()));
    }

    const ComicFormat format = detectFormat(path);
    auto readerRes = createReader(format);
    if (!readerRes.isOk()) {
        return readerRes;
    }

    auto reader = std::move(readerRes).value();
    auto openRes = reader->open(path);
    if (!openRes.isOk()) {
        return Result<std::unique_ptr<IArchiveReader>>::err(openRes.error());
    }

    return Result<std::unique_ptr<IArchiveReader>>::ok(std::move(reader));
}

Result<std::size_t> ArchiveEngine::extractPagesToDirectory(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDir,
    std::stop_token stopToken) {
    return ComicMerger::extractPagesToFolder(archivePath, outputDir, {}, stopToken);
}

Result<std::size_t> ArchiveEngine::mergeComicsToCbz(
    const std::vector<std::filesystem::path>& inputPaths,
    const std::filesystem::path& outputCbzPath,
    std::stop_token stopToken) {
    MergeOptions options;
    options.inputPaths = inputPaths;
    options.outputPath = outputCbzPath;
    options.outputFormat = ComicFormat::CBZ;
    options.saveChapterBookmarks = true;
    options.overwriteExisting = true;

    auto res = ComicMerger::merge(options, {}, stopToken);
    if (!res) {
        return Result<std::size_t>::err(res.error());
    }
    return Result<std::size_t>::ok(res.value().totalPages);
}

} // namespace mavira::core::archive
