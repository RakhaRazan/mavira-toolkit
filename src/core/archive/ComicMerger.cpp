#include "core/archive/ComicMerger.hpp"

#include "core/archive/ArchiveEngine.hpp"
#include "core/archive/PdfDocumentReader.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "core/imaging/NativeOcrEngine.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <archive.h>
#include <archive_entry.h>
#include <turbojpeg.h>
#include <zip.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace mavira::core::archive {

namespace {

using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;
using mavira::core::imaging::ImageDecoder;
using mavira::core::imaging::ImageFormat;
using mavira::data::db::ComicRecord;
using mavira::data::db::DatabaseManager;
using mavira::data::fs::FileManager;

[[nodiscard]] std::int64_t currentUnixSeconds() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

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

[[nodiscard]] std::filesystem::path stripTrailingSeparator(std::filesystem::path p) {
    p.make_preferred();
    if (!p.has_filename() && p.has_parent_path() && p != p.root_path()) {
        p = p.parent_path();
    }
    return p;
}

void notifyExplorerDirUpdated(const std::filesystem::path& dir) noexcept {
    if (dir.empty()) {
        return;
    }
    std::filesystem::path normalized = stripTrailingSeparator(dir);
    normalized.make_preferred();
    std::wstring w = normalized.native();
    if (w.rfind(L"\\\\?\\", 0) == 0) {
        w.erase(0, 4);
    }
    if (!w.empty()) {
        ::SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, w.c_str(), nullptr);
    }
}

void notifyExplorerForDirs(const std::set<std::filesystem::path>& dirs) noexcept {
    for (const auto& d : dirs) {
        notifyExplorerDirUpdated(d);
    }
}

[[nodiscard]] std::string volumeDisplayTitle(const std::filesystem::path& rawPath) {
    const std::filesystem::path path = stripTrailingSeparator(rawPath);
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(FileManager::toWin32LongPath(path), ec) && !ec;
    std::string stem = isDir
        ? FileManager::wideToUtf8(path.filename().native())
        : FileManager::wideToUtf8(path.stem().native());
    if (stem.empty()) {
        stem = FileManager::wideToUtf8(path.filename().native());
    }
    stem = FileManager::sanitizeWin32Name(stem);
    if (stem.empty()) {
        stem = "Volume";
    }
    return stem;
}

[[nodiscard]] std::string formatPrefixedEntryName(
    std::size_t volumeNumber1Based,
    std::size_t pageNumber1Based,
    std::string_view extUtf8) {
    std::ostringstream oss;
    oss << 'V' << std::setw(3) << std::setfill('0') << volumeNumber1Based
        << '_' << std::setw(4) << std::setfill('0') << pageNumber1Based
        << extUtf8;
    return oss.str();
}

[[nodiscard]] std::string extensionFromImageBytesOrName(
    const std::string& logicalName,
    ComicFormat sourceFormat) {
    if (sourceFormat == ComicFormat::PDF) {
        return ".jpg";
    }
    const std::filesystem::path namePath(FileManager::utf8ToWide(logicalName));
    std::string ext = toLowerAscii(FileManager::wideToUtf8(namePath.extension().native()));
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" ||
        ext == ".bmp" || ext == ".gif" || ext == ".avif" || ext == ".tif" || ext == ".tiff") {
        return ext;
    }
    return ".jpg";
}

[[nodiscard]] std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char ch : text) {
        switch (ch) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] std::string buildMergedComicInfoXml(
    const std::filesystem::path& outputPath,
    std::size_t totalPages,
    const std::vector<ChapterBookmark>& bookmarks) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        << "<ComicInfo xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        << "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n"
        << "  <Title>" << xmlEscape(volumeDisplayTitle(outputPath)) << "</Title>\n"
        << "  <PageCount>" << totalPages << "</PageCount>\n";
    if (!bookmarks.empty()) {
        oss << "  <Pages>\n";
        for (const auto& bm : bookmarks) {
            oss << "    <Page Image=\"" << bm.startPageIndex << "\" Bookmark=\""
                << xmlEscape(bm.title) << "\" />\n";
        }
        oss << "  </Pages>\n";
    }
    oss << "</ComicInfo>\n";
    return oss.str();
}

/// Escapes a UTF-8 string for a PDF literal string `(...)`.
[[nodiscard]] std::string pdfEscapeString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char ch : text) {
        if (ch == '(' || ch == ')' || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch >= 0x20 && ch <= 0x7E) {
            out.push_back(static_cast<char>(ch));
        } else {
            char oct[8];
            std::snprintf(oct, sizeof(oct), "\\%03o", static_cast<unsigned int>(ch));
            out += oct;
        }
    }
    return out;
}

/// Reads a single page from `reader`. For PDF sources, renders and encodes as high-quality JPEG
/// so CBZ/CBR/PDF outputs remain compact rather than storing huge raw BMPs.
[[nodiscard]] Result<std::vector<std::uint8_t>> readPageForStreaming(
    IArchiveReader& reader,
    ComicFormat sourceFormat,
    std::size_t pageIndex,
    int jpegQuality,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::Cancelled, "Operation cancelled");
    }

    if (sourceFormat == ComicFormat::PDF) {
        if (auto* pdfReader = dynamic_cast<PdfDocumentReader*>(&reader)) {
            if (auto rawRes = pdfReader->tryExtractRawPageImage(pageIndex, stopToken); rawRes.isOk()) {
                return rawRes;
            }
            if (auto nativeRes = pdfReader->tryExtractNativePageBitmap(pageIndex, stopToken); nativeRes.isOk()) {
                return ImageDecoder::encodeJpeg(nativeRes.value(), jpegQuality);
            }
            auto frameRes = pdfReader->renderPage(pageIndex, 0, 0, stopToken);
            if (!frameRes) {
                return Result<std::vector<std::uint8_t>>::err(frameRes.error());
            }
            return ImageDecoder::encodeJpeg(frameRes.value(), jpegQuality);
        }
    }

    return reader.readPageBytes(pageIndex, stopToken);
}

struct JpegInfo {
    int width{0};
    int height{0};
    int colorspace{TJCS_RGB};
};

[[nodiscard]] std::optional<JpegInfo> inspectJpegHeader(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4 || ImageDecoder::detectFormat(bytes) != ImageFormat::JPEG) {
        return std::nullopt;
    }

    tjhandle handle = ::tjInitDecompress();
    if (handle == nullptr) {
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int subsamp = 0;
    int colorspace = 0;
    const int rc = ::tjDecompressHeader3(
        handle,
        bytes.data(),
        static_cast<unsigned long>(bytes.size()),
        &width,
        &height,
        &subsamp,
        &colorspace);
    ::tjDestroy(handle);

    if (rc != 0 || width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return JpegInfo{width, height, colorspace};
}

struct PdfEncodedImage {
    int width{0};
    int height{0};
    std::string colorSpaceName{"/DeviceRGB"};
    std::string filterName{"/DCTDecode"};
    std::string decodeParms{};
    std::vector<std::uint8_t> streamBytes{};
};

[[nodiscard]] Result<PdfEncodedImage> encodePdfImageStream(
    std::vector<std::uint8_t> rawBytes,
    int jpgQuality,
    bool preferLosslessFlate,
    std::stop_token stopToken) {
    if (auto jpgInfo = inspectJpegHeader(rawBytes); jpgInfo.has_value()) {
        PdfEncodedImage out;
        out.width = jpgInfo->width;
        out.height = jpgInfo->height;
        if (jpgInfo->colorspace == TJCS_GRAY) {
            out.colorSpaceName = "/DeviceGray";
        } else if (jpgInfo->colorspace == TJCS_CMYK || jpgInfo->colorspace == TJCS_YCCK) {
            out.colorSpaceName = "/DeviceCMYK";
        } else {
            out.colorSpaceName = "/DeviceRGB";
        }
        out.filterName = "/DCTDecode";
        out.streamBytes = std::move(rawBytes);
        return Result<PdfEncodedImage>::ok(std::move(out));
    }

    auto frameRes = ImageDecoder::decode(rawBytes, 0, 0, stopToken);
    rawBytes.clear();
    rawBytes.shrink_to_fit();
    if (!frameRes) {
        return Result<PdfEncodedImage>::err(frameRes.error());
    }
    const auto& frame = frameRes.value();
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);

    if (preferLosslessFlate && w > 0 && h > 0) {
        const std::uint32_t srcStride = frame.effectiveStride();

        bool isGrayscale = true;
        for (int y = 0; y < h && isGrayscale; ++y) {
            const std::uint8_t* srcRow = frame.pixels.data() + static_cast<std::size_t>(y) * srcStride;
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* px = srcRow + static_cast<std::size_t>(x) * 4u;
                if (px[0] != px[1] || px[1] != px[2]) {
                    isGrayscale = false;
                    break;
                }
            }
        }

        if (isGrayscale) {
            const std::size_t filteredRowBytes = 1u + static_cast<std::size_t>(w);
            std::vector<std::uint8_t> filtered(filteredRowBytes * static_cast<std::size_t>(h));

            for (int y = 0; y < h; ++y) {
                const std::uint8_t* srcRow = frame.pixels.data() + static_cast<std::size_t>(y) * srcStride;
                std::uint8_t* dstRow = filtered.data() + static_cast<std::size_t>(y) * filteredRowBytes;
                dstRow[0] = 1; // PNG Sub filter
                std::uint8_t prevGray = 0;
                for (int x = 0; x < w; ++x) {
                    const std::uint8_t gray = srcRow[static_cast<std::size_t>(x) * 4u];
                    dstRow[1u + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(gray - prevGray);
                    prevGray = gray;
                }
            }

            uLongf compLen = ::compressBound(static_cast<uLong>(filtered.size()));
            std::vector<std::uint8_t> compressed(compLen);
            if (::compress2(
                    compressed.data(),
                    &compLen,
                    filtered.data(),
                    static_cast<uLong>(filtered.size()),
                    2) == Z_OK) {
                compressed.resize(compLen);
                PdfEncodedImage out;
                out.width = w;
                out.height = h;
                out.colorSpaceName = "/DeviceGray";
                out.filterName = "/FlateDecode";
                out.decodeParms = "/DecodeParms << /Predictor 15 /Colors 1 /BitsPerComponent 8 /Columns " +
                                  std::to_string(w) + " >> ";
                out.streamBytes = std::move(compressed);
                return Result<PdfEncodedImage>::ok(std::move(out));
            }
        } else {
            const std::size_t rowRgbBytes = static_cast<std::size_t>(w) * 3u;
            const std::size_t filteredRowBytes = 1u + rowRgbBytes;
            std::vector<std::uint8_t> filtered(filteredRowBytes * static_cast<std::size_t>(h));

            for (int y = 0; y < h; ++y) {
                const std::uint8_t* srcRow = frame.pixels.data() + static_cast<std::size_t>(y) * srcStride;
                std::uint8_t* dstRow = filtered.data() + static_cast<std::size_t>(y) * filteredRowBytes;
                dstRow[0] = 1; // PNG Sub filter
                std::uint8_t prevR = 0;
                std::uint8_t prevG = 0;
                std::uint8_t prevB = 0;
                for (int x = 0; x < w; ++x) {
                    const std::uint8_t r = srcRow[static_cast<std::size_t>(x) * 4u + 0u];
                    const std::uint8_t g = srcRow[static_cast<std::size_t>(x) * 4u + 1u];
                    const std::uint8_t b = srcRow[static_cast<std::size_t>(x) * 4u + 2u];
                    const std::size_t outIdx = 1u + static_cast<std::size_t>(x) * 3u;
                    dstRow[outIdx + 0u] = static_cast<std::uint8_t>(r - prevR);
                    dstRow[outIdx + 1u] = static_cast<std::uint8_t>(g - prevG);
                    dstRow[outIdx + 2u] = static_cast<std::uint8_t>(b - prevB);
                    prevR = r;
                    prevG = g;
                    prevB = b;
                }
            }

            uLongf compLen = ::compressBound(static_cast<uLong>(filtered.size()));
            std::vector<std::uint8_t> compressed(compLen);
            if (::compress2(
                    compressed.data(),
                    &compLen,
                    filtered.data(),
                    static_cast<uLong>(filtered.size()),
                    2) == Z_OK) {
                compressed.resize(compLen);
                PdfEncodedImage out;
                out.width = w;
                out.height = h;
                out.colorSpaceName = "/DeviceRGB";
                out.filterName = "/FlateDecode";
                out.decodeParms = "/DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns " +
                                  std::to_string(w) + " >> ";
                out.streamBytes = std::move(compressed);
                return Result<PdfEncodedImage>::ok(std::move(out));
            }
        }
    }

    auto encRes = ImageDecoder::encodeJpeg(frame, jpgQuality);
    if (!encRes) {
        return Result<PdfEncodedImage>::err(encRes.error());
    }
    PdfEncodedImage out;
    out.width = w;
    out.height = h;
    out.colorSpaceName = "/DeviceRGB";
    out.filterName = "/DCTDecode";
    out.streamBytes = std::move(encRes).value();
    return Result<PdfEncodedImage>::ok(std::move(out));
}

/// Opens a new `libzip` archive for writing using Win32 wide long-path support.
[[nodiscard]] Result<zip_t*> openZipForWrite(const std::filesystem::path& path) {
    const std::wstring wide = FileManager::toWin32LongPath(path);

    zip_error_t err;
    ::zip_error_init(&err);

    zip_source_t* src = ::zip_source_win32w_create(wide.c_str(), 0, -1, &err);
    if (src == nullptr) {
        std::string msg = std::string("zip_source_win32w_create failed: ") + ::zip_error_strerror(&err);
        ::zip_error_fini(&err);
        return Result<zip_t*>::err(ErrorCode::IoError, std::move(msg));
    }

    zip_t* za = ::zip_open_from_source(src, ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (za == nullptr) {
        std::string msg = std::string("zip_open_from_source failed: ") + ::zip_error_strerror(&err);
        ::zip_source_free(src);
        ::zip_error_fini(&err);
        return Result<zip_t*>::err(ErrorCode::IoError, std::move(msg));
    }

    ::zip_error_fini(&err);
    return Result<zip_t*>::ok(za);
}

[[nodiscard]] Result<void> atomicMoveFile(
    const std::filesystem::path& tempPath,
    const std::filesystem::path& destPath,
    bool overwriteExisting) {
    const std::wstring srcWide = FileManager::toWin32LongPath(tempPath);
    const std::wstring dstWide = FileManager::toWin32LongPath(destPath);

    DWORD flags = MOVEFILE_COPY_ALLOWED;
    if (overwriteExisting) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }

    if (!::MoveFileExW(srcWide.c_str(), dstWide.c_str(), flags)) {
        const DWORD lastErr = ::GetLastError();
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(
            ErrorCode::IoError,
            "Failed to move output file to destination (Win32 error " + std::to_string(lastErr) + ")");
    }
    return Result<void>::ok();
}

// =============================================================================
// 1-Page Streaming Session for libzip CBZ Merge
// =============================================================================
struct VolumePlan {
    std::filesystem::path path{};
    ComicFormat format{ComicFormat::Unknown};
    std::string displayTitle{};
    std::size_t pageCount{0};
    std::uint32_t startPageIndex{0};
};

struct ActiveMergeSession {
    std::vector<VolumePlan> volumes{};
    std::size_t totalPages{0};
    int jpegQuality{92};
    MergeProgressCallback progressCallback{};
    std::stop_token stopToken{};

    std::size_t openVolumeIndex{static_cast<std::size_t>(-1)};
    std::unique_ptr<IArchiveReader> activeReader{};
    std::size_t pagesCompleted{0};
    bool cancelled{false};
    std::string lastError{};
};

struct StreamingMergeEntryCtx {
    std::shared_ptr<ActiveMergeSession> session{};
    std::size_t volumeIndex{0};
    std::size_t pageIndex{0};
    std::vector<std::uint8_t> buffer{};
    std::size_t offset{0};
    zip_error_t error{};
};

zip_int64_t streamingMergeSourceCallback(
    void* userdata,
    void* data,
    zip_uint64_t len,
    zip_source_cmd_t cmd) {
    auto* ctx = static_cast<StreamingMergeEntryCtx*>(userdata);
    auto& s = *ctx->session;

    switch (cmd) {
        case ZIP_SOURCE_OPEN: {
            if (s.stopToken.stop_requested()) {
                s.cancelled = true;
                ::zip_error_set(&ctx->error, ZIP_ER_CANCELLED, 0);
                return -1;
            }

            const auto& vol = s.volumes[ctx->volumeIndex];
            if (s.openVolumeIndex != ctx->volumeIndex || s.activeReader == nullptr) {
                // Release previous volume reader before opening the next volume to keep RAM flat.
                s.activeReader.reset();
                s.openVolumeIndex = static_cast<std::size_t>(-1);

                auto readerRes = ArchiveEngine::openArchive(vol.path);
                if (!readerRes) {
                    s.lastError = readerRes.error().message;
                    ::zip_error_set(&ctx->error, ZIP_ER_OPEN, 0);
                    return -1;
                }
                s.activeReader = std::move(readerRes).value();
                s.openVolumeIndex = ctx->volumeIndex;
            }

            auto bytesRes = readPageForStreaming(
                *s.activeReader,
                vol.format,
                ctx->pageIndex,
                s.jpegQuality,
                s.stopToken);
            if (!bytesRes) {
                if (bytesRes.errorCode() == ErrorCode::Cancelled) {
                    s.cancelled = true;
                    ::zip_error_set(&ctx->error, ZIP_ER_CANCELLED, 0);
                } else {
                    s.lastError = bytesRes.error().message;
                    ::zip_error_set(&ctx->error, ZIP_ER_READ, 0);
                }
                return -1;
            }

            ctx->buffer = std::move(bytesRes).value();
            ctx->offset = 0;
            return 0;
        }
        case ZIP_SOURCE_READ: {
            if (s.stopToken.stop_requested()) {
                s.cancelled = true;
                ::zip_error_set(&ctx->error, ZIP_ER_CANCELLED, 0);
                return -1;
            }
            const std::size_t remaining = ctx->buffer.size() - ctx->offset;
            const std::size_t toCopy = (std::min)(remaining, static_cast<std::size_t>(len));
            if (toCopy > 0) {
                std::memcpy(data, ctx->buffer.data() + ctx->offset, toCopy);
                ctx->offset += toCopy;
            }
            return static_cast<zip_int64_t>(toCopy);
        }
        case ZIP_SOURCE_CLOSE: {
            // Immediately release the single-page buffer before libzip moves to the next entry.
            ctx->buffer.clear();
            ctx->buffer.shrink_to_fit();
            ctx->offset = 0;

            ++s.pagesCompleted;
            const auto& vol = s.volumes[ctx->volumeIndex];
            if (ctx->pageIndex + 1 >= vol.pageCount) {
                // Volume finished; release its reader handle immediately.
                s.activeReader.reset();
                s.openVolumeIndex = static_cast<std::size_t>(-1);
            }

            if (s.progressCallback) {
                MergeProgress prog;
                prog.currentVolumeIndex = ctx->volumeIndex;
                prog.totalVolumes = s.volumes.size();
                prog.currentPageInVolume = ctx->pageIndex + 1;
                prog.pagesInCurrentVolume = vol.pageCount;
                prog.totalPagesProcessed = s.pagesCompleted;
                prog.totalPagesEstimated = s.totalPages;
                prog.currentVolumeName = vol.displayTitle;
                s.progressCallback(prog);
            }
            return 0;
        }
        case ZIP_SOURCE_STAT: {
            auto* st = static_cast<zip_stat_t*>(data);
            ::zip_stat_init(st);
            return sizeof(zip_stat_t);
        }
        case ZIP_SOURCE_ERROR: {
            return ::zip_error_to_data(&ctx->error, data, len);
        }
        case ZIP_SOURCE_FREE: {
            ::zip_error_fini(&ctx->error);
            delete ctx;
            return 0;
        }
        case ZIP_SOURCE_SUPPORTS: {
            return ::zip_source_make_command_bitmap(
                ZIP_SOURCE_OPEN,
                ZIP_SOURCE_READ,
                ZIP_SOURCE_CLOSE,
                ZIP_SOURCE_STAT,
                ZIP_SOURCE_ERROR,
                ZIP_SOURCE_FREE,
                -1);
        }
        default:
            ::zip_error_set(&ctx->error, ZIP_ER_OPNOTSUPP, 0);
            return -1;
    }
}

// =============================================================================
// 1-Page Streaming Session for createCbzFromFolder
// =============================================================================
struct FolderCbzSession {
    std::size_t totalFiles{0};
    std::size_t completedFiles{0};
    MergeProgressCallback progressCallback{};
    std::stop_token stopToken{};
    bool cancelled{false};
    std::string lastError{};
};

struct FolderFileSourceCtx {
    std::shared_ptr<FolderCbzSession> session{};
    std::filesystem::path filePath{};
    std::size_t fileIndex{0};
    std::ifstream stream{};
    zip_error_t error{};
};

zip_int64_t folderFileSourceCallback(
    void* userdata,
    void* data,
    zip_uint64_t len,
    zip_source_cmd_t cmd) {
    auto* ctx = static_cast<FolderFileSourceCtx*>(userdata);
    auto& s = *ctx->session;

    switch (cmd) {
        case ZIP_SOURCE_OPEN: {
            if (s.stopToken.stop_requested()) {
                s.cancelled = true;
                ::zip_error_set(&ctx->error, ZIP_ER_CANCELLED, 0);
                return -1;
            }
            const std::wstring wide = FileManager::toWin32LongPath(ctx->filePath);
            ctx->stream.open(wide, std::ios::binary);
            if (!ctx->stream.is_open()) {
                s.lastError = "Failed to open image file: " +
                              FileManager::wideToUtf8(ctx->filePath.native());
                ::zip_error_set(&ctx->error, ZIP_ER_OPEN, 0);
                return -1;
            }
            return 0;
        }
        case ZIP_SOURCE_READ: {
            if (s.stopToken.stop_requested()) {
                s.cancelled = true;
                ::zip_error_set(&ctx->error, ZIP_ER_CANCELLED, 0);
                return -1;
            }
            ctx->stream.read(static_cast<char*>(data), static_cast<std::streamsize>(len));
            const std::streamsize count = ctx->stream.gcount();
            if (count == 0 && ctx->stream.bad()) {
                ::zip_error_set(&ctx->error, ZIP_ER_READ, 0);
                return -1;
            }
            return static_cast<zip_int64_t>(count);
        }
        case ZIP_SOURCE_CLOSE: {
            ctx->stream.close();
            ++s.completedFiles;
            if (s.progressCallback) {
                MergeProgress prog;
                prog.currentVolumeIndex = 0;
                prog.totalVolumes = 1;
                prog.currentPageInVolume = s.completedFiles;
                prog.pagesInCurrentVolume = s.totalFiles;
                prog.totalPagesProcessed = s.completedFiles;
                prog.totalPagesEstimated = s.totalFiles;
                prog.currentVolumeName = FileManager::wideToUtf8(ctx->filePath.filename().native());
                s.progressCallback(prog);
            }
            return 0;
        }
        case ZIP_SOURCE_STAT: {
            auto* st = static_cast<zip_stat_t*>(data);
            ::zip_stat_init(st);
            std::error_code ec;
            const auto sz = std::filesystem::file_size(FileManager::toWin32LongPath(ctx->filePath), ec);
            if (!ec) {
                st->valid |= ZIP_STAT_SIZE;
                st->size = sz;
            }
            return sizeof(zip_stat_t);
        }
        case ZIP_SOURCE_ERROR: {
            return ::zip_error_to_data(&ctx->error, data, len);
        }
        case ZIP_SOURCE_FREE: {
            ctx->stream.close();
            ::zip_error_fini(&ctx->error);
            delete ctx;
            return 0;
        }
        case ZIP_SOURCE_SUPPORTS: {
            return ::zip_source_make_command_bitmap(
                ZIP_SOURCE_OPEN,
                ZIP_SOURCE_READ,
                ZIP_SOURCE_CLOSE,
                ZIP_SOURCE_STAT,
                ZIP_SOURCE_ERROR,
                ZIP_SOURCE_FREE,
                -1);
        }
        default:
            ::zip_error_set(&ctx->error, ZIP_ER_OPNOTSUPP, 0);
            return -1;
    }
}

// =============================================================================
// Shared Plan Builder (opens each input briefly to count pages & build bookmarks)
// =============================================================================
struct PlannedMerge {
    std::vector<VolumePlan> volumes{};
    std::vector<ChapterBookmark> bookmarks{};
    std::vector<std::vector<std::string>> entryNamesPerVolume{};
    std::size_t totalPages{0};
};

[[nodiscard]] Result<PlannedMerge> buildMergePlan(
    const MergeOptions& options,
    std::stop_token stopToken) {
    PlannedMerge plan;
    plan.volumes.reserve(options.inputPaths.size());
    plan.entryNamesPerVolume.reserve(options.inputPaths.size());

    std::size_t runningPageOffset = 0;
    for (std::size_t volIdx = 0; volIdx < options.inputPaths.size(); ++volIdx) {
        if (stopToken.stop_requested()) {
            return Result<PlannedMerge>::err(ErrorCode::Cancelled, "Merge cancelled");
        }

        const auto& inputPath = options.inputPaths[volIdx];
        const ComicFormat fmt = ArchiveEngine::detectFormat(inputPath);
        auto readerRes = ArchiveEngine::openArchive(inputPath);
        if (!readerRes) {
            return Result<PlannedMerge>::err(readerRes.error());
        }

        const auto& reader = *readerRes.value();
        const std::size_t count = reader.pageCount();

        VolumePlan vp;
        vp.path = inputPath;
        vp.format = fmt;
        vp.displayTitle = volumeDisplayTitle(inputPath);
        vp.pageCount = count;
        vp.startPageIndex = static_cast<std::uint32_t>(runningPageOffset);

        std::vector<std::string> names;
        names.reserve(count);
        for (std::size_t p = 0; p < count; ++p) {
            const std::string ext = extensionFromImageBytesOrName(reader.pageName(p), fmt);
            names.push_back(formatPrefixedEntryName(volIdx + 1, p + 1, ext));
        }

        if (count > 0) {
            plan.bookmarks.push_back(ChapterBookmark{vp.startPageIndex, vp.displayTitle});
        }

        runningPageOffset += count;
        plan.volumes.push_back(std::move(vp));
        plan.entryNamesPerVolume.push_back(std::move(names));
    }

    plan.totalPages = runningPageOffset;
    if (plan.totalPages == 0) {
        return Result<PlannedMerge>::err(
            ErrorCode::InvalidArgument, "No readable image pages found across input files");
    }

    return Result<PlannedMerge>::ok(std::move(plan));
}

// =============================================================================
// CBZ Streaming Merger (Zero-Recompression ZIP_CM_STORE Pass-Through)
// =============================================================================
[[nodiscard]] Result<MergeResult> mergeToCbz(
    const MergeOptions& options,
    const PlannedMerge& plan,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    std::error_code ec;
    if (!options.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(options.outputPath.parent_path(), ec);
    }

    const std::filesystem::path tempPath(options.outputPath.native() + L".tmp");
    auto zipRes = openZipForWrite(tempPath);
    if (!zipRes) {
        return Result<MergeResult>::err(zipRes.error());
    }
    zip_t* outZip = zipRes.value();

    auto session = std::make_shared<ActiveMergeSession>();
    session->volumes = plan.volumes;
    session->totalPages = plan.totalPages;
    session->jpegQuality = options.pdfJpegQuality;
    session->progressCallback = progressCallback;
    session->stopToken = stopToken;

    for (std::size_t volIdx = 0; volIdx < plan.volumes.size(); ++volIdx) {
        const auto& vol = plan.volumes[volIdx];
        const auto& names = plan.entryNamesPerVolume[volIdx];
        for (std::size_t p = 0; p < vol.pageCount; ++p) {
            auto* ctx = new StreamingMergeEntryCtx();
            ctx->session = session;
            ctx->volumeIndex = volIdx;
            ctx->pageIndex = p;
            ::zip_error_init(&ctx->error);

            zip_source_t* src = ::zip_source_function(outZip, &streamingMergeSourceCallback, ctx);
            if (src == nullptr) {
                ::zip_error_fini(&ctx->error);
                delete ctx;
                ::zip_discard(outZip);
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::IoError, "zip_source_function failed");
            }

            const zip_int64_t idx = ::zip_file_add(
                outZip,
                names[p].c_str(),
                src,
                ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
            if (idx < 0) {
                ::zip_source_free(src);
                ::zip_discard(outZip);
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::IoError, "zip_file_add failed");
            }

            // Enforce zero-recompression ZIP_CM_STORE pass-through (100% original image quality).
            (void)::zip_set_file_compression(outZip, static_cast<zip_uint64_t>(idx), ZIP_CM_STORE, 0);
        }
    }

    // Embed ComicInfo.xml containing volume start pages as chapter bookmarks.
    std::string comicInfoXml;
    if (options.saveChapterBookmarks) {
        comicInfoXml = buildMergedComicInfoXml(options.outputPath, plan.totalPages, plan.bookmarks);
        zip_source_t* xmlSrc =
            ::zip_source_buffer(outZip, comicInfoXml.data(), comicInfoXml.size(), 0);
        if (xmlSrc != nullptr) {
            const zip_int64_t xmlIdx = ::zip_file_add(
                outZip,
                "ComicInfo.xml",
                xmlSrc,
                ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
            if (xmlIdx >= 0) {
                (void)::zip_set_file_compression(
                    outZip, static_cast<zip_uint64_t>(xmlIdx), ZIP_CM_STORE, 0);
            } else {
                ::zip_source_free(xmlSrc);
            }
        }
    }

    const int closeRc = ::zip_close(outZip);
    session->activeReader.reset();

    if (closeRc != 0) {
        const std::string zipErr = ::zip_strerror(outZip);
        ::zip_discard(outZip);
        std::filesystem::remove(tempPath, ec);
        if (session->cancelled || stopToken.stop_requested()) {
            return Result<MergeResult>::err(ErrorCode::Cancelled, "Merge cancelled");
        }
        return Result<MergeResult>::err(
            ErrorCode::IoError,
            session->lastError.empty()
                ? ("Failed to finalize merged CBZ archive: " + zipErr)
                : session->lastError);
    }

    auto moveRes = atomicMoveFile(tempPath, options.outputPath, options.overwriteExisting);
    if (!moveRes) {
        return Result<MergeResult>::err(moveRes.error());
    }

    MergeResult result;
    result.outputPath = options.outputPath;
    result.outputFormat = ComicFormat::CBZ;
    result.totalPages = plan.totalPages;
    result.totalVolumes = plan.volumes.size();
    result.chapterBookmarks = plan.bookmarks;
    return Result<MergeResult>::ok(std::move(result));
}

// =============================================================================
// PDF 1.4 Streaming Binary Writer (1-Page Pipeline + /Outlines Chapter Bookmarks)
// =============================================================================
[[nodiscard]] Result<MergeResult> mergeToPdf(
    const MergeOptions& options,
    const PlannedMerge& plan,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    std::error_code ec;
    if (!options.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(options.outputPath.parent_path(), ec);
    }

    const std::filesystem::path tempPath(options.outputPath.native() + L".tmp");
    std::ofstream ofs(FileManager::toWin32LongPath(tempPath), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Result<MergeResult>::err(
            ErrorCode::IoError,
            "Failed to open temporary PDF output: " + FileManager::wideToUtf8(tempPath.native()));
    }

    std::uint64_t byteOffset = 0;
    auto writeBytes = [&](const void* ptr, std::size_t len) -> bool {
        if (len == 0) {
            return true;
        }
        ofs.write(static_cast<const char*>(ptr), static_cast<std::streamsize>(len));
        byteOffset += static_cast<std::uint64_t>(len);
        return ofs.good();
    };
    auto writeStr = [&](std::string_view sv) -> bool {
        return writeBytes(sv.data(), sv.size());
    };

    // PDF 1.4 header + binary comment bytes
    const unsigned char headerBytes[] = {
        '%', 'P', 'D', 'F', '-', '1', '.', '4', '\n',
        '%', 0xE2, 0xE3, 0xCF, 0xD3, '\n'
    };
    if (!writeBytes(headerBytes, sizeof(headerBytes))) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF header");
    }

    // Reserve fixed root object IDs:
    // 1 = Catalog, 2 = Pages, 3 = Outlines (if bookmarks present)
    const bool includeOutlines = options.saveChapterBookmarks && !plan.bookmarks.empty();
    const std::uint32_t catalogObjId = 1;
    const std::uint32_t pagesObjId = 2;
    const std::uint32_t outlinesObjId = includeOutlines ? 3u : 0u;
    std::uint32_t nextObjId = includeOutlines ? 4u : 3u;

    std::vector<std::uint64_t> objOffsets(nextObjId, 0);
    auto setObjOffset = [&](std::uint32_t objId, std::uint64_t off) {
        if (objId >= objOffsets.size()) {
            objOffsets.resize(static_cast<std::size_t>(objId) + 1, 0);
        }
        objOffsets[objId] = off;
    };

    std::vector<std::uint32_t> pageObjIds;
    pageObjIds.reserve(plan.totalPages);

    std::size_t pagesProcessed = 0;
    for (std::size_t volIdx = 0; volIdx < plan.volumes.size(); ++volIdx) {
        if (stopToken.stop_requested()) {
            ofs.close();
            std::filesystem::remove(tempPath, ec);
            return Result<MergeResult>::err(ErrorCode::Cancelled, "PDF merge cancelled");
        }

        const auto& vol = plan.volumes[volIdx];
        auto readerRes = ArchiveEngine::openArchive(vol.path);
        if (!readerRes) {
            ofs.close();
            std::filesystem::remove(tempPath, ec);
            return Result<MergeResult>::err(readerRes.error());
        }
        auto reader = std::move(readerRes).value();

        for (std::size_t p = 0; p < vol.pageCount; ++p) {
            if (stopToken.stop_requested()) {
                reader.reset();
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::Cancelled, "PDF merge cancelled");
            }

            auto rawRes = readPageForStreaming(*reader, vol.format, p, options.pdfJpegQuality, stopToken);
            if (!rawRes) {
                reader.reset();
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(rawRes.error());
            }

            auto imgRes = encodePdfImageStream(
                std::move(rawRes).value(), options.pdfJpegQuality, /*preferLosslessFlate=*/true, stopToken);
            if (!imgRes) {
                reader.reset();
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(imgRes.error());
            }
            const int imgWidth = imgRes.value().width;
            const int imgHeight = imgRes.value().height;

            const std::uint32_t imgObjId = nextObjId++;
            const std::uint32_t contentObjId = nextObjId++;
            const std::uint32_t pageObjId = nextObjId++;
            pageObjIds.push_back(pageObjId);

            // 1) Image XObject
            setObjOffset(imgObjId, byteOffset);
            std::ostringstream imgHeader;
            imgHeader << imgObjId << " 0 obj\n"
                      << "<< /Type /XObject /Subtype /Image /Width " << imgWidth
                      << " /Height " << imgHeight
                      << " /ColorSpace " << imgRes.value().colorSpaceName
                      << " /BitsPerComponent 8 /Filter " << imgRes.value().filterName << ' '
                      << imgRes.value().decodeParms
                      << "/Length " << imgRes.value().streamBytes.size()
                      << " >>\nstream\n";
            if (!writeStr(imgHeader.str()) ||
                !writeBytes(imgRes.value().streamBytes.data(), imgRes.value().streamBytes.size()) ||
                !writeStr("\nendstream\nendobj\n")) {
                reader.reset();
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF image object");
            }
            // Immediately free single-page image buffer
            imgRes.value().streamBytes.clear();
            imgRes.value().streamBytes.shrink_to_fit();

            // 2) Content stream
            std::ostringstream contentStream;
            contentStream << "q\n"
                          << imgWidth << " 0 0 " << imgHeight << " 0 0 cm\n"
                          << "/Im0 Do\nQ\n";
            const std::string contentStr = contentStream.str();

            setObjOffset(contentObjId, byteOffset);
            std::ostringstream contentObj;
            contentObj << contentObjId << " 0 obj\n"
                       << "<< /Length " << contentStr.size() << " >>\nstream\n"
                       << contentStr
                       << "endstream\nendobj\n";
            if (!writeStr(contentObj.str())) {
                reader.reset();
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF content stream");
            }

            // 3) Page object
            setObjOffset(pageObjId, byteOffset);
            std::ostringstream pageObj;
            pageObj << pageObjId << " 0 obj\n"
                    << "<< /Type /Page /Parent " << pagesObjId << " 0 R "
                    << "/MediaBox [0 0 " << imgWidth << ' ' << imgHeight << "] "
                    << "/Resources << /XObject << /Im0 " << imgObjId << " 0 R >> >> "
                    << "/Contents " << contentObjId << " 0 R >>\nendobj\n";
            if (!writeStr(pageObj.str())) {
                reader.reset();
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF page object");
            }

            ++pagesProcessed;
            if (progressCallback) {
                MergeProgress prog;
                prog.currentVolumeIndex = volIdx;
                prog.totalVolumes = plan.volumes.size();
                prog.currentPageInVolume = p + 1;
                prog.pagesInCurrentVolume = vol.pageCount;
                prog.totalPagesProcessed = pagesProcessed;
                prog.totalPagesEstimated = plan.totalPages;
                prog.currentVolumeName = vol.displayTitle;
                progressCallback(prog);
            }
        }
        // Release volume reader before opening the next volume
        reader.reset();
    }

    // 4) Chapter Bookmarks (/Outlines)
    if (includeOutlines) {
        std::vector<std::uint32_t> outlineItemIds;
        outlineItemIds.reserve(plan.bookmarks.size());
        for (std::size_t i = 0; i < plan.bookmarks.size(); ++i) {
            outlineItemIds.push_back(nextObjId++);
        }

        for (std::size_t i = 0; i < plan.bookmarks.size(); ++i) {
            const auto& bm = plan.bookmarks[i];
            const std::uint32_t itemObjId = outlineItemIds[i];
            const std::size_t pageIdx =
                (std::min)(static_cast<std::size_t>(bm.startPageIndex), pageObjIds.size() - 1);
            const std::uint32_t targetPageObjId = pageObjIds[pageIdx];

            setObjOffset(itemObjId, byteOffset);
            std::ostringstream itemObj;
            itemObj << itemObjId << " 0 obj\n<< /Title (" << pdfEscapeString(bm.title) << ") "
                    << "/Parent " << outlinesObjId << " 0 R ";
            if (i > 0) {
                itemObj << "/Prev " << outlineItemIds[i - 1] << " 0 R ";
            }
            if (i + 1 < outlineItemIds.size()) {
                itemObj << "/Next " << outlineItemIds[i + 1] << " 0 R ";
            }
            itemObj << "/Dest [" << targetPageObjId << " 0 R /Fit] >>\nendobj\n";
            if (!writeStr(itemObj.str())) {
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF outline item");
            }
        }

        setObjOffset(outlinesObjId, byteOffset);
        std::ostringstream outlinesRoot;
        outlinesRoot << outlinesObjId << " 0 obj\n<< /Type /Outlines "
                     << "/First " << outlineItemIds.front() << " 0 R "
                     << "/Last " << outlineItemIds.back() << " 0 R "
                     << "/Count " << outlineItemIds.size() << " >>\nendobj\n";
        if (!writeStr(outlinesRoot.str())) {
            ofs.close();
            std::filesystem::remove(tempPath, ec);
            return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF outlines root");
        }
    }

    // 5) Pages root (Object 2)
    setObjOffset(pagesObjId, byteOffset);
    std::ostringstream pagesRoot;
    pagesRoot << pagesObjId << " 0 obj\n<< /Type /Pages /Kids [";
    for (std::size_t i = 0; i < pageObjIds.size(); ++i) {
        if (i > 0) {
            pagesRoot << ' ';
        }
        pagesRoot << pageObjIds[i] << " 0 R";
    }
    pagesRoot << "] /Count " << pageObjIds.size() << " >>\nendobj\n";
    if (!writeStr(pagesRoot.str())) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF pages root");
    }

    // 6) Catalog (Object 1)
    setObjOffset(catalogObjId, byteOffset);
    std::ostringstream catalogObj;
    catalogObj << catalogObjId << " 0 obj\n<< /Type /Catalog /Pages " << pagesObjId << " 0 R";
    if (includeOutlines) {
        catalogObj << " /Outlines " << outlinesObjId << " 0 R /PageMode /UseOutlines";
    }
    catalogObj << " >>\nendobj\n";
    if (!writeStr(catalogObj.str())) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF catalog");
    }

    // 7) Cross-reference table & trailer
    const std::uint64_t xrefOffset = byteOffset;
    const std::size_t totalObjCount = objOffsets.size();
    std::ostringstream xref;
    xref << "xref\n0 " << totalObjCount << '\n'
         << "0000000000 65535 f \n";
    for (std::size_t i = 1; i < totalObjCount; ++i) {
        char entry[32];
        std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n",
                      static_cast<unsigned long long>(objOffsets[i]));
        xref << entry;
    }
    xref << "trailer\n<< /Size " << totalObjCount << " /Root " << catalogObjId << " 0 R >>\n"
         << "startxref\n" << xrefOffset << "\n%%EOF\n";
    if (!writeStr(xref.str())) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(ErrorCode::IoError, "Failed writing PDF xref table");
    }

    ofs.flush();
    const bool good = ofs.good();
    ofs.close();
    if (!good) {
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(ErrorCode::IoError, "Failed flushing PDF output");
    }

    auto moveRes = atomicMoveFile(tempPath, options.outputPath, options.overwriteExisting);
    if (!moveRes) {
        return Result<MergeResult>::err(moveRes.error());
    }

    MergeResult result;
    result.outputPath = options.outputPath;
    result.outputFormat = ComicFormat::PDF;
    result.totalPages = plan.totalPages;
    result.totalVolumes = plan.volumes.size();
    result.chapterBookmarks = plan.bookmarks;
    return Result<MergeResult>::ok(std::move(result));
}

// =============================================================================
// CB7 Open-Source Merger via libarchive (7-Zip LZMA2)
// =============================================================================
[[nodiscard]] Result<struct archive*> openCb7ForWrite(
    const std::filesystem::path& tempPath,
    std::string_view compressionMode) {
    struct archive* a = ::archive_write_new();
    if (a == nullptr) {
        return Result<struct archive*>::err(ErrorCode::OutOfMemory, "archive_write_new failed");
    }
    if (::archive_write_set_format_7zip(a) != ARCHIVE_OK) {
        std::string msg = ::archive_error_string(a) ? ::archive_error_string(a) : "archive_write_set_format_7zip failed";
        ::archive_write_free(a);
        return Result<struct archive*>::err(ErrorCode::IoError, std::move(msg));
    }

    const std::string mode = toLowerAscii(compressionMode);
    if (mode == "store") {
        (void)::archive_write_set_options(a, "7zip:compression=store");
    } else if (mode == "max") {
        (void)::archive_write_set_options(a, "7zip:compression=lzma2,compression-level=9");
    } else if (mode == "balanced") {
        (void)::archive_write_set_options(a, "7zip:compression=lzma2,compression-level=5");
    } else {
        (void)::archive_write_set_options(a, "7zip:compression=lzma2");
    }

    const std::wstring wide = FileManager::toWin32LongPath(tempPath);
    if (::archive_write_open_filename_w(a, wide.c_str()) != ARCHIVE_OK) {
        std::string msg = ::archive_error_string(a) ? ::archive_error_string(a) : "archive_write_open_filename_w failed";
        ::archive_write_free(a);
        return Result<struct archive*>::err(ErrorCode::IoError, std::move(msg));
    }
    return Result<struct archive*>::ok(a);
}

[[nodiscard]] Result<struct archive*> openCbtForWrite(const std::filesystem::path& tempPath) {
    struct archive* a = ::archive_write_new();
    if (a == nullptr) {
        return Result<struct archive*>::err(ErrorCode::OutOfMemory, "archive_write_new failed");
    }
    if (::archive_write_set_format_pax_restricted(a) != ARCHIVE_OK) {
        std::string msg = ::archive_error_string(a) ? ::archive_error_string(a) : "archive_write_set_format_pax_restricted failed";
        ::archive_write_free(a);
        return Result<struct archive*>::err(ErrorCode::IoError, std::move(msg));
    }

    const std::wstring wide = FileManager::toWin32LongPath(tempPath);
    if (::archive_write_open_filename_w(a, wide.c_str()) != ARCHIVE_OK) {
        std::string msg = ::archive_error_string(a) ? ::archive_error_string(a) : "archive_write_open_filename_w failed";
        ::archive_write_free(a);
        return Result<struct archive*>::err(ErrorCode::IoError, std::move(msg));
    }
    return Result<struct archive*>::ok(a);
}

[[nodiscard]] Result<void> writeCb7Entry(
    struct archive* a,
    const std::string& entryName,
    const void* data,
    std::size_t size) {
    struct archive_entry* entry = ::archive_entry_new();
    if (entry == nullptr) {
        return Result<void>::err(ErrorCode::OutOfMemory, "archive_entry_new failed");
    }
    ::archive_entry_set_pathname_utf8(entry, entryName.c_str());
    ::archive_entry_set_size(entry, static_cast<la_int64_t>(size));
    ::archive_entry_set_filetype(entry, AE_IFREG);
    ::archive_entry_set_perm(entry, 0644);

    if (::archive_write_header(a, entry) != ARCHIVE_OK) {
        std::string msg = ::archive_error_string(a) ? ::archive_error_string(a) : "archive_write_header failed";
        ::archive_entry_free(entry);
        return Result<void>::err(ErrorCode::IoError, std::move(msg));
    }

    const char* ptr = static_cast<const char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const la_ssize_t written = ::archive_write_data(a, ptr, remaining);
        if (written < 0) {
            std::string msg = ::archive_error_string(a) ? ::archive_error_string(a) : "archive_write_data failed";
            ::archive_entry_free(entry);
            return Result<void>::err(ErrorCode::IoError, std::move(msg));
        }
        ptr += written;
        remaining -= static_cast<std::size_t>(written);
    }

    ::archive_entry_free(entry);
    return Result<void>::ok();
}

[[nodiscard]] Result<MergeResult> mergeToLibArchive(
    const MergeOptions& options,
    const PlannedMerge& plan,
    ComicFormat targetFormat,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    std::error_code ec;
    if (!options.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(FileManager::toWin32LongPath(options.outputPath.parent_path()), ec);
    }

    const std::filesystem::path tempPath(options.outputPath.native() + L".tmp");
    std::filesystem::remove(tempPath, ec);

    auto archRes = (targetFormat == ComicFormat::CBT)
        ? openCbtForWrite(tempPath)
        : openCb7ForWrite(tempPath, "lzma2");
    if (!archRes) {
        return Result<MergeResult>::err(archRes.error());
    }
    struct archive* outArch = archRes.value();

    std::size_t pagesProcessed = 0;
    for (std::size_t volIdx = 0; volIdx < plan.volumes.size(); ++volIdx) {
        if (stopToken.stop_requested()) {
            ::archive_write_close(outArch);
            ::archive_write_free(outArch);
            std::filesystem::remove(tempPath, ec);
            return Result<MergeResult>::err(ErrorCode::Cancelled, "Archive merge cancelled");
        }

        const auto& vol = plan.volumes[volIdx];
        const auto& names = plan.entryNamesPerVolume[volIdx];
        auto readerRes = ArchiveEngine::openArchive(vol.path);
        if (!readerRes) {
            ::archive_write_close(outArch);
            ::archive_write_free(outArch);
            std::filesystem::remove(tempPath, ec);
            return Result<MergeResult>::err(readerRes.error());
        }
        auto reader = std::move(readerRes).value();

        for (std::size_t p = 0; p < vol.pageCount; ++p) {
            if (stopToken.stop_requested()) {
                ::archive_write_close(outArch);
                ::archive_write_free(outArch);
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(ErrorCode::Cancelled, "Archive merge cancelled");
            }

            auto bytesRes = readPageForStreaming(*reader, vol.format, p, options.pdfJpegQuality, stopToken);
            if (!bytesRes) {
                ::archive_write_close(outArch);
                ::archive_write_free(outArch);
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(bytesRes.error());
            }

            const auto& buf = bytesRes.value();
            auto wRes = writeCb7Entry(outArch, names[p], buf.data(), buf.size());
            if (!wRes) {
                ::archive_write_close(outArch);
                ::archive_write_free(outArch);
                std::filesystem::remove(tempPath, ec);
                return Result<MergeResult>::err(wRes.error());
            }

            ++pagesProcessed;
            if (progressCallback) {
                MergeProgress prog;
                prog.currentVolumeIndex = volIdx;
                prog.totalVolumes = plan.volumes.size();
                prog.currentPageInVolume = p + 1;
                prog.pagesInCurrentVolume = vol.pageCount;
                prog.totalPagesProcessed = pagesProcessed;
                prog.totalPagesEstimated = plan.totalPages;
                prog.currentVolumeName = vol.displayTitle;
                progressCallback(prog);
            }
        }
    }

    if (options.saveChapterBookmarks) {
        const std::string xml = buildMergedComicInfoXml(options.outputPath, plan.totalPages, plan.bookmarks);
        auto xmlRes = writeCb7Entry(outArch, "ComicInfo.xml", xml.data(), xml.size());
        if (!xmlRes) {
            ::archive_write_close(outArch);
            ::archive_write_free(outArch);
            std::filesystem::remove(tempPath, ec);
            return Result<MergeResult>::err(xmlRes.error());
        }
    }

    const int closeRc = ::archive_write_close(outArch);
    const std::string closeErr = (closeRc != ARCHIVE_OK && ::archive_error_string(outArch))
        ? std::string(::archive_error_string(outArch))
        : std::string("archive_write_close failed");
    ::archive_write_free(outArch);
    if (closeRc != ARCHIVE_OK) {
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(ErrorCode::IoError, closeErr);
    }

    auto moveRes = atomicMoveFile(tempPath, options.outputPath, options.overwriteExisting);
    if (!moveRes) {
        return Result<MergeResult>::err(moveRes.error());
    }

    MergeResult result;
    result.outputPath = options.outputPath;
    result.outputFormat = targetFormat;
    result.totalPages = plan.totalPages;
    result.totalVolumes = plan.volumes.size();
    result.chapterBookmarks = plan.bookmarks;
    return Result<MergeResult>::ok(std::move(result));
}

struct EpubPageMeta {
    std::string imageEntryPath{};
    std::string imageHref{};
    std::string xhtmlEntryPath{};
    std::string xhtmlHref{};
    std::string mimeType{};
    int width{1200};
    int height{1800};
};

void probeEpubImageInfo(
    const std::vector<std::uint8_t>& bytes,
    std::string_view extHint,
    int& outWidth,
    int& outHeight,
    std::string& outMime) {
    outWidth = 1200;
    outHeight = 1800;

    if (auto jpg = inspectJpegHeader(bytes); jpg.has_value()) {
        outWidth = jpg->width;
        outHeight = jpg->height;
        outMime = "image/jpeg";
        return;
    }

    if (bytes.size() >= 24 &&
        bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
        bytes[12] == 'I' && bytes[13] == 'H' && bytes[14] == 'D' && bytes[15] == 'R') {
        const std::uint32_t w = (static_cast<std::uint32_t>(bytes[16]) << 24) |
                                (static_cast<std::uint32_t>(bytes[17]) << 16) |
                                (static_cast<std::uint32_t>(bytes[18]) << 8) |
                                static_cast<std::uint32_t>(bytes[19]);
        const std::uint32_t h = (static_cast<std::uint32_t>(bytes[20]) << 24) |
                                (static_cast<std::uint32_t>(bytes[21]) << 16) |
                                (static_cast<std::uint32_t>(bytes[22]) << 8) |
                                static_cast<std::uint32_t>(bytes[23]);
        if (w > 0 && h > 0) {
            outWidth = static_cast<int>(w);
            outHeight = static_cast<int>(h);
        }
        outMime = "image/png";
        return;
    }

    const ImageFormat detFmt = ImageDecoder::detectFormat(bytes);
    if (detFmt == ImageFormat::WEBP) {
        outMime = "image/webp";
    } else if (detFmt == ImageFormat::PNG) {
        outMime = "image/png";
    } else if (detFmt == ImageFormat::GIF) {
        outMime = "image/gif";
    } else if (detFmt == ImageFormat::BMP) {
        outMime = "image/bmp";
    } else if (detFmt == ImageFormat::JPEG) {
        outMime = "image/jpeg";
    } else {
        const std::string extLow = toLowerAscii(extHint);
        if (extLow == ".png") outMime = "image/png";
        else if (extLow == ".webp") outMime = "image/webp";
        else if (extLow == ".gif") outMime = "image/gif";
        else if (extLow == ".bmp") outMime = "image/bmp";
        else if (extLow == ".avif") outMime = "image/avif";
        else if (extLow == ".tif" || extLow == ".tiff") outMime = "image/tiff";
        else outMime = "image/jpeg";
    }

    auto dec = ImageDecoder::decode(bytes);
    if (dec && dec.value().width > 0 && dec.value().height > 0) {
        outWidth = static_cast<int>(dec.value().width);
        outHeight = static_cast<int>(dec.value().height);
    }
}

[[nodiscard]] std::string formatEpubPageBaseName(std::size_t pageNumber1Based, int digits) {
    std::ostringstream oss;
    oss << "page_" << std::setw(digits) << std::setfill('0') << pageNumber1Based;
    return oss.str();
}

[[nodiscard]] int epubDigitsForCount(std::size_t totalPages) noexcept {
    int digits = 3;
    std::size_t v = totalPages;
    int needed = 1;
    while (v >= 10) {
        v /= 10;
        ++needed;
    }
    return (std::max)(digits, needed);
}

[[nodiscard]] std::string buildEpubContainerXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
           "  <rootfiles>\n"
           "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
           "  </rootfiles>\n"
           "</container>\n";
}

[[nodiscard]] std::string buildEpubPageXhtml(
    std::size_t pageNumber1Based,
    const EpubPageMeta& meta) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE html>\n"
        << "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
        << "<head>\n"
        << "  <meta charset=\"utf-8\"/>\n"
        << "  <meta name=\"viewport\" content=\"width=" << meta.width << ", height=" << meta.height << "\"/>\n"
        << "  <title>Page " << pageNumber1Based << "</title>\n"
        << "  <style type=\"text/css\">\n"
        << "    html, body { margin: 0; padding: 0; width: 100%; height: 100%; background: #000000; overflow: hidden; }\n"
        << "    img { display: block; width: 100%; height: 100%; object-fit: contain; }\n"
        << "  </style>\n"
        << "</head>\n"
        << "<body>\n"
        << "  <img src=\"" << xmlEscape(meta.imageHref) << "\" alt=\"Page " << pageNumber1Based << "\"/>\n"
        << "</body>\n"
        << "</html>\n";
    return oss.str();
}

[[nodiscard]] std::string buildEpubTocNcx(
    std::string_view title,
    const std::vector<EpubPageMeta>& pages,
    const std::vector<ChapterBookmark>& bookmarks) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
        << "  <head>\n"
        << "    <meta name=\"dtb:uid\" content=\"urn:mavira:comic\"/>\n"
        << "    <meta name=\"dtb:depth\" content=\"1\"/>\n"
        << "    <meta name=\"dtb:totalPageCount\" content=\"" << pages.size() << "\"/>\n"
        << "    <meta name=\"dtb:maxPageNumber\" content=\"" << pages.size() << "\"/>\n"
        << "  </head>\n"
        << "  <docTitle><text>" << xmlEscape(title) << "</text></docTitle>\n"
        << "  <navMap>\n";

    if (!bookmarks.empty() && !pages.empty()) {
        for (std::size_t i = 0; i < bookmarks.size(); ++i) {
            const std::size_t idx = (std::min)(
                static_cast<std::size_t>(bookmarks[i].startPageIndex), pages.size() - 1);
            oss << "    <navPoint id=\"navPoint-" << (i + 1) << "\" playOrder=\"" << (i + 1) << "\">\n"
                << "      <navLabel><text>" << xmlEscape(bookmarks[i].title) << "</text></navLabel>\n"
                << "      <content src=\"" << xmlEscape(pages[idx].xhtmlHref) << "\"/>\n"
                << "    </navPoint>\n";
        }
    } else if (!pages.empty()) {
        oss << "    <navPoint id=\"navPoint-1\" playOrder=\"1\">\n"
            << "      <navLabel><text>" << xmlEscape(title) << "</text></navLabel>\n"
            << "      <content src=\"" << xmlEscape(pages.front().xhtmlHref) << "\"/>\n"
            << "    </navPoint>\n";
    }

    oss << "  </navMap>\n"
        << "</ncx>\n";
    return oss.str();
}

[[nodiscard]] std::string buildEpubNavXhtml(
    std::string_view title,
    const std::vector<EpubPageMeta>& pages,
    const std::vector<ChapterBookmark>& bookmarks) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE html>\n"
        << "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
        << "<head>\n"
        << "  <meta charset=\"utf-8\"/>\n"
        << "  <title>" << xmlEscape(title) << "</title>\n"
        << "</head>\n"
        << "<body>\n"
        << "  <nav epub:type=\"toc\" id=\"toc\">\n"
        << "    <h1>" << xmlEscape(title) << "</h1>\n"
        << "    <ol>\n";
    if (!bookmarks.empty() && !pages.empty()) {
        for (const auto& bm : bookmarks) {
            const std::size_t idx = (std::min)(
                static_cast<std::size_t>(bm.startPageIndex), pages.size() - 1);
            oss << "      <li><a href=\"" << xmlEscape(pages[idx].xhtmlHref) << "\">"
                << xmlEscape(bm.title) << "</a></li>\n";
        }
    } else if (!pages.empty()) {
        oss << "      <li><a href=\"" << xmlEscape(pages.front().xhtmlHref) << "\">"
            << xmlEscape(title) << "</a></li>\n";
    }
    oss << "    </ol>\n"
        << "  </nav>\n"
        << "</body>\n"
        << "</html>\n";
    return oss.str();
}

[[nodiscard]] std::string buildEpubContentOpf(
    std::string_view title,
    const std::vector<EpubPageMeta>& pages) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" "
        << "prefix=\"rendition: http://www.idpf.org/vocab/rendition/#\">\n"
        << "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        << "    <dc:identifier id=\"bookid\">urn:mavira:comic</dc:identifier>\n"
        << "    <dc:title>" << xmlEscape(title) << "</dc:title>\n"
        << "    <dc:language>en</dc:language>\n"
        << "    <meta property=\"dcterms:modified\">2026-01-01T00:00:00Z</meta>\n"
        << "    <meta property=\"rendition:layout\">pre-paginated</meta>\n"
        << "    <meta property=\"rendition:orientation\">auto</meta>\n"
        << "    <meta property=\"rendition:spread\">auto</meta>\n"
        << "  </metadata>\n"
        << "  <manifest>\n"
        << "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
        << "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n";
    for (std::size_t i = 0; i < pages.size(); ++i) {
        oss << "    <item id=\"page_" << (i + 1) << "\" href=\"" << xmlEscape(pages[i].xhtmlHref)
            << "\" media-type=\"application/xhtml+xml\"/>\n";
        oss << "    <item id=\"img_" << (i + 1) << "\" href=\"" << xmlEscape(pages[i].imageHref)
            << "\" media-type=\"" << xmlEscape(pages[i].mimeType) << "\"";
        if (i == 0) {
            oss << " properties=\"cover-image\"";
        }
        oss << "/>\n";
    }
    oss << "  </manifest>\n"
        << "  <spine toc=\"ncx\">\n";
    for (std::size_t i = 0; i < pages.size(); ++i) {
        oss << "    <itemref idref=\"page_" << (i + 1) << "\"/>\n";
    }
    oss << "  </spine>\n"
        << "</package>\n";
    return oss.str();
}

[[nodiscard]] Result<void> addZipBufferEntry(
    zip_t* outZip,
    const char* entryName,
    const void* data,
    std::size_t size,
    zip_int32_t compMethod,
    zip_uint32_t compLevel,
    bool utf8NameFlag = true) {
    zip_source_t* src = ::zip_source_buffer(outZip, data, size, 0);
    if (src == nullptr) {
        return Result<void>::err(ErrorCode::IoError, "zip_source_buffer failed");
    }
    const zip_flags_t flags = ZIP_FL_OVERWRITE | (utf8NameFlag ? ZIP_FL_ENC_UTF_8 : 0);
    const zip_int64_t idx = ::zip_file_add(outZip, entryName, src, flags);
    if (idx < 0) {
        ::zip_source_free(src);
        return Result<void>::err(ErrorCode::IoError, std::string("zip_file_add failed for ") + entryName);
    }
    (void)::zip_set_file_compression(outZip, static_cast<zip_uint64_t>(idx), compMethod, compLevel);
    return Result<void>::ok();
}

[[nodiscard]] Result<MergeResult> mergeToEpub(
    const MergeOptions& options,
    const PlannedMerge& plan,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    std::error_code ec;
    if (!options.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(options.outputPath.parent_path(), ec);
    }

    const std::filesystem::path tempPath(options.outputPath.native() + L".tmp");
    auto zipRes = openZipForWrite(tempPath);
    if (!zipRes) {
        return Result<MergeResult>::err(zipRes.error());
    }
    zip_t* outZip = zipRes.value();

    auto abortZip = [&](ErrorCode code, std::string msg) {
        ::zip_discard(outZip);
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(code, std::move(msg));
    };

    // Keep all buffer payloads alive until zip_close() completes.
    const std::string mimetypeStr = "application/epub+zip";
    const std::string containerXml = buildEpubContainerXml();
    if (auto r = addZipBufferEntry(
            outZip, "mimetype", mimetypeStr.data(), mimetypeStr.size(), ZIP_CM_STORE, 0, false);
        !r) {
        return abortZip(r.errorCode(), r.error().message);
    }
    if (auto r = addZipBufferEntry(
            outZip, "META-INF/container.xml", containerXml.data(), containerXml.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortZip(r.errorCode(), r.error().message);
    }

    const int digits = epubDigitsForCount(plan.totalPages);
    std::vector<std::vector<std::uint8_t>> imageBuffers;
    imageBuffers.reserve(plan.totalPages);
    std::vector<EpubPageMeta> pageMetas;
    pageMetas.reserve(plan.totalPages);
    std::vector<std::string> pageXhtmls;
    pageXhtmls.reserve(plan.totalPages);

    std::size_t pagesProcessed = 0;
    for (std::size_t volIdx = 0; volIdx < plan.volumes.size(); ++volIdx) {
        if (stopToken.stop_requested()) {
            return abortZip(ErrorCode::Cancelled, "EPUB merge cancelled");
        }
        const auto& vol = plan.volumes[volIdx];
        const auto& names = plan.entryNamesPerVolume[volIdx];
        auto readerRes = ArchiveEngine::openArchive(vol.path);
        if (!readerRes) {
            return abortZip(readerRes.errorCode(), readerRes.error().message);
        }
        auto reader = std::move(readerRes).value();

        for (std::size_t p = 0; p < vol.pageCount; ++p) {
            if (stopToken.stop_requested()) {
                return abortZip(ErrorCode::Cancelled, "EPUB merge cancelled");
            }
            auto bytesRes = readPageForStreaming(*reader, vol.format, p, options.pdfJpegQuality, stopToken);
            if (!bytesRes) {
                return abortZip(bytesRes.errorCode(), bytesRes.error().message);
            }
            imageBuffers.push_back(std::move(bytesRes).value());

            const std::size_t page1 = pagesProcessed + 1;
            const std::string extHint = FileManager::wideToUtf8(
                std::filesystem::path(FileManager::utf8ToWide(names[p])).extension().native());
            EpubPageMeta meta;
            probeEpubImageInfo(imageBuffers.back(), extHint, meta.width, meta.height, meta.mimeType);
            const std::string base = formatEpubPageBaseName(page1, digits);
            meta.imageHref = "images/" + base + (extHint.empty() ? ".jpg" : extHint);
            meta.imageEntryPath = "OEBPS/" + meta.imageHref;
            meta.xhtmlHref = base + ".xhtml";
            meta.xhtmlEntryPath = "OEBPS/" + meta.xhtmlHref;

            pageXhtmls.push_back(buildEpubPageXhtml(page1, meta));
            if (auto r = addZipBufferEntry(
                    outZip,
                    meta.imageEntryPath.c_str(),
                    imageBuffers.back().data(),
                    imageBuffers.back().size(),
                    ZIP_CM_STORE,
                    0);
                !r) {
                return abortZip(r.errorCode(), r.error().message);
            }
            if (auto r = addZipBufferEntry(
                    outZip,
                    meta.xhtmlEntryPath.c_str(),
                    pageXhtmls.back().data(),
                    pageXhtmls.back().size(),
                    ZIP_CM_DEFLATE,
                    6);
                !r) {
                return abortZip(r.errorCode(), r.error().message);
            }
            pageMetas.push_back(std::move(meta));

            ++pagesProcessed;
            if (progressCallback) {
                MergeProgress prog;
                prog.currentVolumeIndex = volIdx;
                prog.totalVolumes = plan.volumes.size();
                prog.currentPageInVolume = p + 1;
                prog.pagesInCurrentVolume = vol.pageCount;
                prog.totalPagesProcessed = pagesProcessed;
                prog.totalPagesEstimated = plan.totalPages;
                prog.currentVolumeName = vol.displayTitle;
                progressCallback(prog);
            }
        }
    }

    const std::string title = volumeDisplayTitle(options.outputPath);
    const std::vector<ChapterBookmark> emptyBms;
    const auto& effectiveBms = options.saveChapterBookmarks ? plan.bookmarks : emptyBms;
    const std::string tocNcx = buildEpubTocNcx(title, pageMetas, effectiveBms);
    const std::string navXhtml = buildEpubNavXhtml(title, pageMetas, effectiveBms);
    const std::string contentOpf = buildEpubContentOpf(title, pageMetas);

    if (auto r = addZipBufferEntry(
            outZip, "OEBPS/toc.ncx", tocNcx.data(), tocNcx.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortZip(r.errorCode(), r.error().message);
    }
    if (auto r = addZipBufferEntry(
            outZip, "OEBPS/nav.xhtml", navXhtml.data(), navXhtml.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortZip(r.errorCode(), r.error().message);
    }
    if (auto r = addZipBufferEntry(
            outZip, "OEBPS/content.opf", contentOpf.data(), contentOpf.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortZip(r.errorCode(), r.error().message);
    }

    const int closeRc = ::zip_close(outZip);
    imageBuffers.clear();
    pageXhtmls.clear();
    if (closeRc != 0) {
        const std::string zipErr = ::zip_strerror(outZip);
        ::zip_discard(outZip);
        std::filesystem::remove(tempPath, ec);
        return Result<MergeResult>::err(ErrorCode::IoError, "Failed to finalize merged EPUB archive: " + zipErr);
    }

    auto moveRes = atomicMoveFile(tempPath, options.outputPath, options.overwriteExisting);
    if (!moveRes) {
        return Result<MergeResult>::err(moveRes.error());
    }

    MergeResult result;
    result.outputPath = options.outputPath;
    result.outputFormat = ComicFormat::EPUB;
    result.totalPages = plan.totalPages;
    result.totalVolumes = plan.volumes.size();
    result.chapterBookmarks = plan.bookmarks;
    return Result<MergeResult>::ok(std::move(result));
}

} // namespace

Result<MergeResult> ComicMerger::merge(
    const MergeOptions& options,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    if (options.inputPaths.empty()) {
        return Result<MergeResult>::err(
            ErrorCode::InvalidArgument, "At least one input comic/PDF file is required");
    }
    if (options.outputPath.empty()) {
        return Result<MergeResult>::err(
            ErrorCode::InvalidArgument, "Output path cannot be empty");
    }
    if (!options.overwriteExisting) {
        std::error_code ec;
        if (std::filesystem::exists(FileManager::toWin32LongPath(options.outputPath), ec) && !ec) {
            return Result<MergeResult>::err(
                ErrorCode::AlreadyExists, "Output file already exists");
        }
    }

    auto planRes = buildMergePlan(options, stopToken);
    if (!planRes) {
        return Result<MergeResult>::err(planRes.error());
    }

    Result<MergeResult> mergeRes = Result<MergeResult>::err(
        ErrorCode::UnsupportedFormat, "Unsupported merge output format");

    switch (options.outputFormat) {
        case ComicFormat::CBZ:
            mergeRes = mergeToCbz(options, planRes.value(), progressCallback, stopToken);
            break;
        case ComicFormat::CB7:
        case ComicFormat::CBT:
            mergeRes = mergeToLibArchive(
                options, planRes.value(), options.outputFormat, progressCallback, stopToken);
            break;
        case ComicFormat::EPUB:
            mergeRes = mergeToEpub(options, planRes.value(), progressCallback, stopToken);
            break;
        case ComicFormat::PDF:
            mergeRes = mergeToPdf(options, planRes.value(), progressCallback, stopToken);
            break;
        default:
            return Result<MergeResult>::err(
                ErrorCode::UnsupportedFormat,
                "Merge output format must be CBZ, CB7, CBT, EPUB, or PDF");
    }

    if (!mergeRes) {
        return mergeRes;
    }

    if (options.saveChapterBookmarks && options.database != nullptr && options.database->isOpen()) {
        auto persistRes = persistChapterBookmarks(
            *options.database,
            mergeRes.value().outputPath,
            mergeRes.value().outputFormat,
            mergeRes.value().totalPages,
            mergeRes.value().chapterBookmarks);
        if (persistRes) {
            mergeRes.value().savedComicId = persistRes.value();
        }
    }

    return mergeRes;
}

Result<std::size_t> ComicMerger::extractPagesToFolder(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputFolder,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    if (archivePath.empty() || outputFolder.empty()) {
        return Result<std::size_t>::err(
            ErrorCode::InvalidArgument, "Archive path or output folder is empty");
    }

    auto readerRes = ArchiveEngine::openArchive(archivePath);
    if (!readerRes) {
        return Result<std::size_t>::err(readerRes.error());
    }

    std::error_code ec;
    std::filesystem::create_directories(FileManager::toWin32LongPath(outputFolder), ec);
    if (ec) {
        return Result<std::size_t>::err(
            ErrorCode::IoError, "Failed to create output folder: " + ec.message());
    }

    const ComicFormat srcFormat = ArchiveEngine::detectFormat(archivePath);
    const std::string volTitle = volumeDisplayTitle(archivePath);
    auto& reader = *readerRes.value();
    const std::size_t total = reader.pageCount();
    std::size_t extracted = 0;

    for (std::size_t i = 0; i < total; ++i) {
        if (stopToken.stop_requested()) {
            return Result<std::size_t>::err(ErrorCode::Cancelled, "Page extraction cancelled");
        }

        auto bytesRes = readPageForStreaming(reader, srcFormat, i, 92, stopToken);
        if (!bytesRes) {
            return Result<std::size_t>::err(bytesRes.error());
        }

        if (bytesRes.value().empty()) {
            continue;
        }

        const std::string logicalName = reader.pageName(i);
        std::filesystem::path relName =
            std::filesystem::path(FileManager::utf8ToWide(logicalName)).filename();
        if (relName.empty() || srcFormat == ComicFormat::PDF) {
            std::ostringstream oss;
            oss << "page_" << std::setw(5) << std::setfill('0') << (i + 1)
                << (srcFormat == ComicFormat::PDF ? ".jpg" : ".png");
            relName = FileManager::utf8ToWide(oss.str());
        }

        std::filesystem::path outPath = outputFolder / relName;
        if (std::filesystem::exists(FileManager::toWin32LongPath(outPath), ec)) {
            std::ostringstream oss;
            oss << std::setw(5) << std::setfill('0') << (i + 1) << '_'
                << FileManager::wideToUtf8(relName.native());
            outPath = outputFolder / FileManager::utf8ToWide(oss.str());
        }

        {
            std::ofstream ofs(FileManager::toWin32LongPath(outPath), std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                return Result<std::size_t>::err(
                    ErrorCode::IoError,
                    "Failed to open output page file: " + FileManager::wideToUtf8(outPath.native()));
            }
            const auto& data = bytesRes.value();
            ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!ofs.good()) {
                return Result<std::size_t>::err(ErrorCode::IoError, "Failed writing extracted page");
            }
        }

        ++extracted;
        if (progressCallback) {
            MergeProgress prog;
            prog.currentVolumeIndex = 0;
            prog.totalVolumes = 1;
            prog.currentPageInVolume = extracted;
            prog.pagesInCurrentVolume = total;
            prog.totalPagesProcessed = extracted;
            prog.totalPagesEstimated = total;
            prog.currentVolumeName = volTitle;
            progressCallback(prog);
        }
    }

    return Result<std::size_t>::ok(extracted);
}

Result<std::size_t> ComicMerger::createCbzFromFolder(
    const std::filesystem::path& inputFolder,
    const std::filesystem::path& outputCbzPath,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    if (inputFolder.empty() || outputCbzPath.empty()) {
        return Result<std::size_t>::err(
            ErrorCode::InvalidArgument, "Input folder or output CBZ path is empty");
    }

    std::error_code ec;
    const std::filesystem::path longInput(FileManager::toWin32LongPath(inputFolder));
    if (!std::filesystem::is_directory(longInput, ec) || ec) {
        return Result<std::size_t>::err(
            ErrorCode::NotFound,
            "Input folder does not exist: " + FileManager::wideToUtf8(inputFolder.native()));
    }

    struct FolderImage {
        std::filesystem::path fullPath;
        std::string relativeName;
    };
    std::vector<FolderImage> images;

    for (std::filesystem::recursive_directory_iterator it(
             longInput, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end && !ec;
         it.increment(ec)) {
        if (stopToken.stop_requested()) {
            return Result<std::size_t>::err(ErrorCode::Cancelled, "CBZ creation cancelled");
        }
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }
        const auto& p = it->path();
        if (!FileManager::isSupportedImageExtension(p)) {
            continue;
        }
        std::filesystem::path rel = std::filesystem::relative(p, longInput, ec);
        std::string relUtf8 = FileManager::wideToUtf8((ec ? p.filename() : rel).native());
        std::replace(relUtf8.begin(), relUtf8.end(), '\\', '/');
        images.push_back(FolderImage{p, std::move(relUtf8)});
    }

    if (images.empty()) {
        return Result<std::size_t>::err(
            ErrorCode::InvalidArgument, "No supported image files found in folder");
    }

    std::sort(images.begin(), images.end(), [](const FolderImage& a, const FolderImage& b) {
        return FileManager::naturalCompare(a.relativeName, b.relativeName) < 0;
    });

    if (!outputCbzPath.parent_path().empty()) {
        std::filesystem::create_directories(outputCbzPath.parent_path(), ec);
    }

    const std::filesystem::path tempPath(outputCbzPath.native() + L".tmp");
    auto zipRes = openZipForWrite(tempPath);
    if (!zipRes) {
        return Result<std::size_t>::err(zipRes.error());
    }
    zip_t* outZip = zipRes.value();

    auto session = std::make_shared<FolderCbzSession>();
    session->totalFiles = images.size();
    session->progressCallback = progressCallback;
    session->stopToken = stopToken;

    for (std::size_t i = 0; i < images.size(); ++i) {
        auto* ctx = new FolderFileSourceCtx();
        ctx->session = session;
        ctx->filePath = images[i].fullPath;
        ctx->fileIndex = i;
        ::zip_error_init(&ctx->error);

        zip_source_t* src = ::zip_source_function(outZip, &folderFileSourceCallback, ctx);
        if (src == nullptr) {
            ::zip_error_fini(&ctx->error);
            delete ctx;
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<std::size_t>::err(ErrorCode::IoError, "zip_source_function failed");
        }

        const zip_int64_t idx = ::zip_file_add(
            outZip,
            images[i].relativeName.c_str(),
            src,
            ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
        if (idx < 0) {
            ::zip_source_free(src);
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<std::size_t>::err(ErrorCode::IoError, "zip_file_add failed");
        }

        (void)::zip_set_file_compression(outZip, static_cast<zip_uint64_t>(idx), ZIP_CM_STORE, 0);
    }

    const int closeRc = ::zip_close(outZip);
    if (closeRc != 0) {
        const std::string zipErr = ::zip_strerror(outZip);
        ::zip_discard(outZip);
        std::filesystem::remove(tempPath, ec);
        if (session->cancelled || stopToken.stop_requested()) {
            return Result<std::size_t>::err(ErrorCode::Cancelled, "CBZ creation cancelled");
        }
        return Result<std::size_t>::err(
            ErrorCode::IoError,
            session->lastError.empty()
                ? ("Failed to finalize CBZ archive: " + zipErr)
                : session->lastError);
    }

    auto moveRes = atomicMoveFile(tempPath, outputCbzPath, /*overwriteExisting=*/true);
    if (!moveRes) {
        return Result<std::size_t>::err(moveRes.error());
    }

    return Result<std::size_t>::ok(images.size());
}

Result<MergeResult> ComicMerger::convertComicBook(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputPath,
    ComicFormat targetFormat,
    int pdfJpegQuality,
    bool overwriteExisting,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    if (inputPath.empty() || outputPath.empty()) {
        return Result<MergeResult>::err(
            ErrorCode::InvalidArgument, "Input path and output path cannot be empty");
    }

    if (targetFormat != ComicFormat::CBZ &&
        targetFormat != ComicFormat::CB7 &&
        targetFormat != ComicFormat::CBT &&
        targetFormat != ComicFormat::EPUB &&
        targetFormat != ComicFormat::PDF) {
        return Result<MergeResult>::err(
            ErrorCode::UnsupportedFormat,
            "Target conversion format must be CBZ, CB7, CBT, EPUB, or PDF");
    }

    const ComicFormat srcFormat = ArchiveEngine::detectFormat(inputPath);
    if (srcFormat == ComicFormat::Unknown) {
        return Result<MergeResult>::err(
            ErrorCode::UnsupportedFormat,
            "Unsupported input comic format: " + FileManager::wideToUtf8(inputPath.native()));
    }

    MergeOptions opts;
    opts.inputPaths = {inputPath};
    opts.outputPath = outputPath;
    opts.outputFormat = targetFormat;
    opts.saveChapterBookmarks = false;
    opts.overwriteExisting = overwriteExisting;
    opts.pdfJpegQuality = pdfJpegQuality;

    return merge(opts, progressCallback, stopToken);
}

Result<std::size_t> ComicMerger::batchConvertImages(
    const std::vector<std::filesystem::path>& inputFiles,
    const std::filesystem::path& outputDir,
    ImageFormat targetFormat,
    std::uint32_t maxWidth,
    int quality,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    if (inputFiles.empty() || outputDir.empty()) {
        return Result<std::size_t>::err(
            ErrorCode::InvalidArgument, "Input file list or output directory is empty");
    }

    std::string targetExt;
    switch (targetFormat) {
        case ImageFormat::JPEG: targetExt = ".jpg"; break;
        case ImageFormat::PNG:  targetExt = ".png"; break;
        case ImageFormat::WEBP: targetExt = ".webp"; break;
        default:
            return Result<std::size_t>::err(
                ErrorCode::UnsupportedFormat,
                "Batch image conversion target format must be JPEG, PNG, or WEBP");
    }

    std::error_code ec;
    std::filesystem::create_directories(FileManager::toWin32LongPath(outputDir), ec);
    if (ec) {
        return Result<std::size_t>::err(
            ErrorCode::IoError, "Failed to create output directory: " + ec.message());
    }

    std::size_t converted = 0;
    const std::size_t total = inputFiles.size();

    for (std::size_t i = 0; i < total; ++i) {
        if (stopToken.stop_requested()) {
            return Result<std::size_t>::err(ErrorCode::Cancelled, "Batch image conversion cancelled");
        }

        const auto& inPath = inputFiles[i];
        const std::wstring longIn = FileManager::toWin32LongPath(inPath);
        std::ifstream ifs(longIn, std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) {
            return Result<std::size_t>::err(
                ErrorCode::NotFound,
                "Failed to open input image: " + FileManager::wideToUtf8(inPath.native()));
        }

        const std::streamsize fileSize = ifs.tellg();
        if (fileSize <= 0) {
            return Result<std::size_t>::err(
                ErrorCode::DecodeFailed,
                "Input image is empty: " + FileManager::wideToUtf8(inPath.native()));
        }
        ifs.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> rawBytes(static_cast<std::size_t>(fileSize));
        if (!ifs.read(reinterpret_cast<char*>(rawBytes.data()), fileSize)) {
            return Result<std::size_t>::err(
                ErrorCode::IoError,
                "Failed reading input image: " + FileManager::wideToUtf8(inPath.native()));
        }
        ifs.close();

        auto decodeRes = ImageDecoder::decode(rawBytes, maxWidth, 0, stopToken);
        if (!decodeRes) {
            return Result<std::size_t>::err(decodeRes.error());
        }

        Result<std::vector<std::uint8_t>> encodeRes =
            Result<std::vector<std::uint8_t>>::err(ErrorCode::UnsupportedFormat, "Unsupported format");
        switch (targetFormat) {
            case ImageFormat::JPEG:
                encodeRes = ImageDecoder::encodeJpeg(decodeRes.value(), quality);
                break;
            case ImageFormat::PNG:
                encodeRes = ImageDecoder::encodePng(decodeRes.value());
                break;
            case ImageFormat::WEBP:
                encodeRes = ImageDecoder::encodeWebP(decodeRes.value(), static_cast<float>(quality));
                break;
            default:
                break;
        }
        if (!encodeRes) {
            return Result<std::size_t>::err(encodeRes.error());
        }

        std::string stem = FileManager::wideToUtf8(inPath.stem().native());
        if (stem.empty()) {
            stem = "image_" + std::to_string(i + 1);
        }
        std::filesystem::path outPath = outputDir / FileManager::utf8ToWide(stem + targetExt);
        if (std::filesystem::exists(FileManager::toWin32LongPath(outPath), ec)) {
            outPath = outputDir / FileManager::utf8ToWide(
                stem + "_" + std::to_string(i + 1) + targetExt);
        }

        const std::filesystem::path tempPath(outPath.native() + L".tmp");
        {
            std::ofstream ofs(FileManager::toWin32LongPath(tempPath), std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                return Result<std::size_t>::err(
                    ErrorCode::IoError,
                    "Failed to open output image: " + FileManager::wideToUtf8(outPath.native()));
            }
            const auto& outBytes = encodeRes.value();
            ofs.write(reinterpret_cast<const char*>(outBytes.data()),
                      static_cast<std::streamsize>(outBytes.size()));
            if (!ofs.good()) {
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<std::size_t>::err(ErrorCode::IoError, "Failed writing converted image");
            }
        }

        auto moveRes = atomicMoveFile(tempPath, outPath, /*overwriteExisting=*/true);
        if (!moveRes) {
            return Result<std::size_t>::err(moveRes.error());
        }

        ++converted;
        if (progressCallback) {
            MergeProgress prog;
            prog.currentVolumeIndex = 0;
            prog.totalVolumes = 1;
            prog.currentPageInVolume = converted;
            prog.pagesInCurrentVolume = total;
            prog.totalPagesProcessed = converted;
            prog.totalPagesEstimated = total;
            prog.currentVolumeName = FileManager::wideToUtf8(inPath.filename().native());
            progressCallback(prog);
        }
    }

    return Result<std::size_t>::ok(converted);
}

Result<std::int64_t> ComicMerger::persistChapterBookmarks(
    DatabaseManager& db,
    const std::filesystem::path& mergedOutputPath,
    ComicFormat outputFormat,
    std::size_t totalPages,
    const std::vector<ChapterBookmark>& bookmarks) {
    if (!db.isOpen()) {
        return Result<std::int64_t>::err(ErrorCode::DatabaseError, "Database is not open");
    }

    const std::int64_t nowUnix = currentUnixSeconds();
    std::int64_t comicId = 0;

    if (auto existing = db.getComicByPath(mergedOutputPath); existing && existing.value().has_value()) {
        ComicRecord rec = *existing.value();
        rec.format = outputFormat;
        rec.pageCount = static_cast<std::uint32_t>(totalPages);
        std::error_code ec;
        if (const auto sz = std::filesystem::file_size(FileManager::toWin32LongPath(mergedOutputPath), ec); !ec) {
            rec.fileSizeBytes = sz;
        }
        auto upRes = db.upsertComic(rec);
        if (!upRes) {
            return Result<std::int64_t>::err(upRes.error());
        }
        comicId = upRes.value();
    } else {
        ComicRecord rec;
        rec.filePath = mergedOutputPath;
        rec.folderPath = mergedOutputPath.parent_path();
        rec.title = volumeDisplayTitle(mergedOutputPath);
        rec.format = outputFormat;
        rec.pageCount = static_cast<std::uint32_t>(totalPages);
        rec.addedUnix = nowUnix;
        std::error_code ec;
        if (const auto sz = std::filesystem::file_size(FileManager::toWin32LongPath(mergedOutputPath), ec); !ec) {
            rec.fileSizeBytes = sz;
        }
        auto upRes = db.upsertComic(rec);
        if (!upRes) {
            return Result<std::int64_t>::err(upRes.error());
        }
        comicId = upRes.value();
    }

    for (const auto& bm : bookmarks) {
        auto bmRes = db.addBookmark(comicId, bm.startPageIndex, bm.title, nowUnix);
        if (!bmRes) {
            return Result<std::int64_t>::err(bmRes.error());
        }
    }

    return Result<std::int64_t>::ok(comicId);
}

// =============================================================================
// Unified Tool Execution Pipeline (`runToolJob`)
// =============================================================================
namespace {

using mavira::core::ToolExecutionOptions;
using mavira::core::imaging::NativeOcrEngine;

[[nodiscard]] bool needsImageProcessing(const ToolExecutionOptions& opts) {
    return ImageDecoder::requiresImageReencode(opts) || opts.stripImageMetadata;
}

[[nodiscard]] ImageFormat imageFormatFromString(std::string_view fmt) noexcept {
    const std::string low = toLowerAscii(fmt);
    if (low == "jpg" || low == "jpeg") return ImageFormat::JPEG;
    if (low == "png") return ImageFormat::PNG;
    if (low == "webp") return ImageFormat::WEBP;
    return ImageFormat::Unknown;
}

[[nodiscard]] std::string extForImageFormat(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::PNG:  return ".png";
        case ImageFormat::WEBP: return ".webp";
        case ImageFormat::JPEG:
        default:                return ".jpg";
    }
}

[[nodiscard]] std::string resolveEntryExt(
    const std::string& logicalName,
    ComicFormat srcFmt,
    const ToolExecutionOptions& opts) {
    const ImageFormat target = imageFormatFromString(opts.targetImageFormat);
    if (target != ImageFormat::Unknown) {
        return extForImageFormat(target);
    }
    return extensionFromImageBytesOrName(logicalName, srcFmt);
}

[[nodiscard]] std::pair<zip_int32_t, zip_uint32_t> resolveZipCompression(
    std::string_view compressionMode) noexcept {
    const std::string mode = toLowerAscii(compressionMode);
    if (mode == "balanced") {
        return {ZIP_CM_DEFLATE, 6};
    }
    if (mode == "max") {
        return {ZIP_CM_DEFLATE, 9};
    }
    return {ZIP_CM_STORE, 0};
}

[[nodiscard]] std::wstring toLowerWideKey(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch >= L'A' && ch <= L'Z') {
            out.push_back(static_cast<wchar_t>(ch - L'A' + L'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] std::optional<std::filesystem::path> resolveIfExistsPath(
    const std::filesystem::path& candidate,
    std::string_view ifExistsPolicy,
    bool& outOverwrite,
    std::set<std::wstring>* claimedPaths = nullptr) {
    outOverwrite = false;
    std::error_code ec;

    auto isTaken = [&](const std::filesystem::path& p) -> bool {
        const std::wstring longP = FileManager::toWin32LongPath(p);
        if (std::filesystem::exists(longP, ec) && !ec) {
            return true;
        }
        if (claimedPaths != nullptr) {
            return claimedPaths->contains(toLowerWideKey(longP));
        }
        return false;
    };
    auto claimPath = [&](const std::filesystem::path& p) {
        if (claimedPaths != nullptr) {
            claimedPaths->insert(toLowerWideKey(FileManager::toWin32LongPath(p)));
        }
    };

    if (!isTaken(candidate)) {
        outOverwrite = true;
        claimPath(candidate);
        return candidate;
    }

    const std::string policy = toLowerAscii(ifExistsPolicy);
    if (policy == "overwrite") {
        outOverwrite = true;
        claimPath(candidate);
        return candidate;
    }
    if (policy == "skip") {
        return std::nullopt;
    }

    // "rename" (default): append " (1)", " (2)", ...
    const std::filesystem::path parent = candidate.parent_path();
    const std::wstring stem = candidate.stem().native();
    const std::wstring ext = candidate.extension().native();
    for (int i = 1; i < 100000; ++i) {
        std::filesystem::path next = parent / (stem + L" (" + std::to_wstring(i) + L")" + ext);
        if (!isTaken(next)) {
            outOverwrite = false;
            claimPath(next);
            return next;
        }
    }
    outOverwrite = true;
    claimPath(candidate);
    return candidate;
}

[[nodiscard]] std::filesystem::path resolveBaseOutputDir(
    const std::filesystem::path& rawInputPath,
    const std::filesystem::path& rawRootInputDir,
    const ToolExecutionOptions& opts) {
    const std::filesystem::path inputPath = stripTrailingSeparator(rawInputPath);
    const std::filesystem::path rootInputDir = stripTrailingSeparator(rawRootInputDir);
    std::filesystem::path baseDir;
    if (toLowerAscii(opts.outputFolderMode) == "custom" && !opts.customOutputDir.empty()) {
        baseDir = stripTrailingSeparator(std::filesystem::path(opts.customOutputDir));
    } else {
        std::error_code ec;
        if (std::filesystem::is_directory(FileManager::toWin32LongPath(inputPath), ec) && !ec) {
            baseDir = inputPath.parent_path().empty() ? inputPath : inputPath.parent_path();
        } else {
            baseDir = inputPath.parent_path();
        }
    }

    if (opts.keepSubfolderStructure && !rootInputDir.empty()) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(inputPath.parent_path(), rootInputDir, ec);
        if (!ec && !rel.empty() && rel != L".") {
            baseDir /= rel;
        }
    }
    return baseDir;
}

struct DiscoveredInput {
    std::filesystem::path path{};
    std::filesystem::path rootDir{};
};

[[nodiscard]] bool isAllowedComicFile(
    const std::filesystem::path& p,
    const std::vector<std::string>& allowedExtensions) {
    std::string ext = toLowerAscii(FileManager::wideToUtf8(p.extension().native()));
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    if (ext.empty()) {
        return false;
    }
    if (allowedExtensions.empty()) {
        return ArchiveEngine::detectFormat(p) != ComicFormat::Unknown;
    }
    for (const auto& allowed : allowedExtensions) {
        std::string a = toLowerAscii(allowed);
        if (!a.empty() && a.front() == '.') {
            a.erase(a.begin());
        }
        if (a == ext) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<DiscoveredInput> discoverComicInputs(
    const ToolExecutionOptions& opts,
    bool allowFolderAsVolume) {
    std::vector<DiscoveredInput> out;
    std::error_code ec;
    for (const auto& rawW : opts.inputPaths) {
        if (rawW.empty()) continue;
        const std::filesystem::path p(rawW);
        const std::wstring longP = FileManager::toWin32LongPath(p);
        if (std::filesystem::is_directory(longP, ec) && !ec) {
            std::vector<DiscoveredInput> dirItems;
            if (opts.recursiveFolders) {
                for (std::filesystem::recursive_directory_iterator it(
                         longP, std::filesystem::directory_options::skip_permission_denied, ec),
                     end;
                     it != end && !ec;
                     it.increment(ec)) {
                    if (it->is_regular_file(ec) && !ec &&
                        isAllowedComicFile(it->path(), opts.allowedExtensions)) {
                        dirItems.push_back(DiscoveredInput{it->path(), p});
                    }
                }
            } else {
                for (std::filesystem::directory_iterator it(
                         longP, std::filesystem::directory_options::skip_permission_denied, ec),
                     end;
                     it != end && !ec;
                     it.increment(ec)) {
                    if (it->is_regular_file(ec) && !ec &&
                        isAllowedComicFile(it->path(), opts.allowedExtensions)) {
                        dirItems.push_back(DiscoveredInput{it->path(), p});
                    }
                }
            }
            if (!dirItems.empty()) {
                std::sort(dirItems.begin(), dirItems.end(), [](const DiscoveredInput& a, const DiscoveredInput& b) {
                    return FileManager::naturalCompare(a.path.native(), b.path.native()) < 0;
                });
                out.insert(out.end(), std::make_move_iterator(dirItems.begin()), std::make_move_iterator(dirItems.end()));
            } else if (allowFolderAsVolume) {
                out.push_back(DiscoveredInput{p, p.parent_path()});
            }
        } else if (std::filesystem::is_regular_file(longP, ec) && !ec) {
            out.push_back(DiscoveredInput{p, p.parent_path()});
        }
    }
    return out;
}

[[nodiscard]] std::vector<DiscoveredInput> discoverImageInputs(const ToolExecutionOptions& opts) {
    std::vector<DiscoveredInput> out;
    std::error_code ec;
    for (const auto& rawW : opts.inputPaths) {
        if (rawW.empty()) continue;
        const std::filesystem::path p(rawW);
        const std::wstring longP = FileManager::toWin32LongPath(p);
        if (std::filesystem::is_directory(longP, ec) && !ec) {
            std::vector<DiscoveredInput> dirItems;
            if (opts.recursiveFolders) {
                for (std::filesystem::recursive_directory_iterator it(
                         longP, std::filesystem::directory_options::skip_permission_denied, ec),
                     end;
                     it != end && !ec;
                     it.increment(ec)) {
                    if (it->is_regular_file(ec) && !ec &&
                        FileManager::isSupportedImageExtension(it->path())) {
                        dirItems.push_back(DiscoveredInput{it->path(), p});
                    }
                }
            } else {
                for (std::filesystem::directory_iterator it(
                         longP, std::filesystem::directory_options::skip_permission_denied, ec),
                     end;
                     it != end && !ec;
                     it.increment(ec)) {
                    if (it->is_regular_file(ec) && !ec &&
                        FileManager::isSupportedImageExtension(it->path())) {
                        dirItems.push_back(DiscoveredInput{it->path(), p});
                    }
                }
            }
            std::sort(dirItems.begin(), dirItems.end(), [](const DiscoveredInput& a, const DiscoveredInput& b) {
                return FileManager::naturalCompare(a.path.native(), b.path.native()) < 0;
            });
            out.insert(out.end(), std::make_move_iterator(dirItems.begin()), std::make_move_iterator(dirItems.end()));
        } else if (std::filesystem::is_regular_file(longP, ec) && !ec) {
            out.push_back(DiscoveredInput{p, p.parent_path()});
        }
    }
    return out;
}

template <typename Fn>
[[nodiscard]] Result<void> runParallelIndexed(
    std::size_t count,
    int requestedThreads,
    std::stop_token stopToken,
    Fn&& fn) {
    if (count == 0) {
        return Result<void>::ok();
    }
    const std::size_t workers = static_cast<std::size_t>(
        std::clamp(requestedThreads, 1, static_cast<int>((std::min<std::size_t>)(count, 16))));

    if (workers <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            if (stopToken.stop_requested()) {
                return Result<void>::err(ErrorCode::Cancelled, "Operation cancelled");
            }
            auto res = fn(i);
            if (!res) {
                return res;
            }
        }
        return Result<void>::ok();
    }

    std::atomic<std::size_t> nextIdx{0};
    std::atomic<bool> hasError{false};
    std::mutex errMutex;
    mavira::core::Error firstErr{ErrorCode::Ok, {}};

    {
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (std::size_t t = 0; t < workers; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    if (hasError.load(std::memory_order_acquire) || stopToken.stop_requested()) {
                        return;
                    }
                    const std::size_t idx = nextIdx.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= count) {
                        return;
                    }
                    auto res = fn(idx);
                    if (!res) {
                        bool expected = false;
                        if (hasError.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                            std::lock_guard<std::mutex> lock(errMutex);
                            firstErr = res.error();
                        }
                        return;
                    }
                }
            });
        }
    }

    if (hasError.load(std::memory_order_acquire)) {
        return Result<void>::err(firstErr);
    }
    if (stopToken.stop_requested()) {
        return Result<void>::err(ErrorCode::Cancelled, "Operation cancelled");
    }
    return Result<void>::ok();
}

[[nodiscard]] Result<std::vector<std::uint8_t>> readFileBytes(const std::filesystem::path& p) {
    const std::wstring longP = FileManager::toWin32LongPath(p);
    std::ifstream ifs(longP, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::NotFound, "Failed to open file: " + FileManager::wideToUtf8(p.native()));
    }
    const std::streamsize sz = ifs.tellg();
    if (sz <= 0) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::DecodeFailed, "File is empty: " + FileManager::wideToUtf8(p.native()));
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(bytes.data()), sz)) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::IoError, "Failed reading file: " + FileManager::wideToUtf8(p.native()));
    }
    return Result<std::vector<std::uint8_t>>::ok(std::move(bytes));
}

[[nodiscard]] Result<void> writeFileBytesAtomic(
    const std::filesystem::path& destPath,
    const void* data,
    std::size_t size,
    bool overwriteExisting) {
    if (size == 0 || data == nullptr) {
        return Result<void>::err(
            ErrorCode::DecodeFailed, "Refusing to write empty 0-byte output file");
    }
    std::error_code ec;
    if (!destPath.parent_path().empty()) {
        std::filesystem::create_directories(FileManager::toWin32LongPath(destPath.parent_path()), ec);
    }
    const std::filesystem::path tempPath(destPath.native() + L".tmp");
    {
        std::ofstream ofs(FileManager::toWin32LongPath(tempPath), std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return Result<void>::err(
                ErrorCode::IoError,
                "Failed to open temp file: " + FileManager::wideToUtf8(tempPath.native()));
        }
        ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!ofs.good()) {
            ofs.close();
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(ErrorCode::IoError, "Failed writing output file");
        }
    }
    return atomicMoveFile(tempPath, destPath, overwriteExisting);
}

struct PageSourceRef {
    std::size_t volumeIndex{0};
    std::size_t pageIndex{0};
    std::filesystem::path sourcePath{};
    ComicFormat sourceFormat{ComicFormat::Unknown};
    std::string entryName{};
    bool isStandaloneImage{false};
};

[[nodiscard]] std::string_view trimAscii(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

[[nodiscard]] std::optional<std::size_t> parsePositiveSizeT(std::string_view sv) noexcept {
    sv = trimAscii(sv);
    if (sv.empty()) {
        return std::nullopt;
    }
    std::size_t val = 0;
    const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size() || val == 0) {
        return std::nullopt;
    }
    return val;
}

[[nodiscard]] Result<std::vector<std::size_t>> parsePageRange(
    std::string_view rangeStr,
    std::size_t totalPages) {
    const std::string_view trimmed = trimAscii(rangeStr);
    if (trimmed.empty()) {
        std::vector<std::size_t> all(totalPages);
        for (std::size_t i = 0; i < totalPages; ++i) {
            all[i] = i;
        }
        return Result<std::vector<std::size_t>>::ok(std::move(all));
    }

    std::set<std::size_t> selected;
    std::size_t pos = 0;
    bool sawToken = false;
    while (pos <= trimmed.size()) {
        const std::size_t sep = trimmed.find_first_of(",;", pos);
        const std::string_view token = trimAscii(
            (sep == std::string_view::npos) ? trimmed.substr(pos) : trimmed.substr(pos, sep - pos));
        if (!token.empty()) {
            sawToken = true;
            const std::size_t dash = token.find('-');
            if (dash == std::string_view::npos) {
                auto pOpt = parsePositiveSizeT(token);
                if (!pOpt.has_value()) {
                    return Result<std::vector<std::size_t>>::err(
                        ErrorCode::InvalidArgument,
                        "Invalid page number in range: " + std::string(token));
                }
                if (*pOpt <= totalPages) {
                    selected.insert(*pOpt - 1);
                }
            } else {
                if (token.find('-', dash + 1) != std::string_view::npos) {
                    return Result<std::vector<std::size_t>>::err(
                        ErrorCode::InvalidArgument,
                        "Invalid range token: " + std::string(token));
                }
                const std::string_view left = trimAscii(token.substr(0, dash));
                const std::string_view right = trimAscii(token.substr(dash + 1));
                if (left.empty() && right.empty()) {
                    return Result<std::vector<std::size_t>>::err(
                        ErrorCode::InvalidArgument,
                        "Empty range bounds in: " + std::string(token));
                }
                std::size_t start1 = 1;
                if (!left.empty()) {
                    auto sOpt = parsePositiveSizeT(left);
                    if (!sOpt.has_value()) {
                        return Result<std::vector<std::size_t>>::err(
                            ErrorCode::InvalidArgument,
                            "Invalid range start in: " + std::string(token));
                    }
                    start1 = *sOpt;
                }
                std::size_t end1 = totalPages;
                if (!right.empty()) {
                    auto eOpt = parsePositiveSizeT(right);
                    if (!eOpt.has_value()) {
                        return Result<std::vector<std::size_t>>::err(
                            ErrorCode::InvalidArgument,
                            "Invalid range end in: " + std::string(token));
                    }
                    end1 = *eOpt;
                }
                if (start1 > end1) {
                    std::swap(start1, end1);
                }
                if (totalPages > 0 && start1 <= totalPages) {
                    const std::size_t clampedEnd = (std::min)(end1, totalPages);
                    for (std::size_t p = start1; p <= clampedEnd; ++p) {
                        selected.insert(p - 1);
                    }
                }
            }
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 1;
    }

    if (!sawToken) {
        return Result<std::vector<std::size_t>>::err(
            ErrorCode::InvalidArgument, "Page range contains no valid page numbers");
    }

    return Result<std::vector<std::size_t>>::ok(
        std::vector<std::size_t>(selected.begin(), selected.end()));
}

[[nodiscard]] std::string formatSequentialPageName(
    std::size_t pageNumber1Based,
    int digits,
    std::string_view extUtf8) {
    std::ostringstream oss;
    oss << std::setw(digits) << std::setfill('0') << pageNumber1Based << extUtf8;
    return oss.str();
}

[[nodiscard]] std::optional<std::uint32_t> probeImageWidthFromBytes(
    const std::vector<std::uint8_t>& bytes) {
    if (auto jpg = inspectJpegHeader(bytes); jpg.has_value() && jpg->width > 0) {
        return static_cast<std::uint32_t>(jpg->width);
    }
    // PNG IHDR width at bytes 16..19 (big-endian)
    if (bytes.size() >= 24 &&
        bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
        bytes[12] == 'I' && bytes[13] == 'H' && bytes[14] == 'D' && bytes[15] == 'R') {
        const std::uint32_t w = (static_cast<std::uint32_t>(bytes[16]) << 24) |
                                (static_cast<std::uint32_t>(bytes[17]) << 16) |
                                (static_cast<std::uint32_t>(bytes[18]) << 8) |
                                static_cast<std::uint32_t>(bytes[19]);
        if (w > 0) {
            return w;
        }
    }
    auto dec = ImageDecoder::decode(bytes);
    if (dec && dec.value().width > 0) {
        return dec.value().width;
    }
    return std::nullopt;
}

[[nodiscard]] Result<std::uint32_t> probeMaxPageWidth(
    const std::vector<PageSourceRef>& pages,
    const ToolExecutionOptions& opts,
    std::stop_token stopToken) {
    std::uint32_t maxWidth = 0;
    std::size_t openVol = static_cast<std::size_t>(-1);
    std::unique_ptr<IArchiveReader> reader;

    for (const auto& ref : pages) {
        if (stopToken.stop_requested()) {
            return Result<std::uint32_t>::err(ErrorCode::Cancelled, "Operation cancelled");
        }
        if (ref.isStandaloneImage) {
            auto rawRes = readFileBytes(ref.sourcePath);
            if (!rawRes) return Result<std::uint32_t>::err(rawRes.error());
            if (auto w = probeImageWidthFromBytes(rawRes.value()); w.has_value()) {
                maxWidth = (std::max)(maxWidth, *w);
            }
            continue;
        }
        if (openVol != ref.volumeIndex || !reader) {
            reader.reset();
            auto rRes = ArchiveEngine::openArchive(ref.sourcePath);
            if (!rRes) return Result<std::uint32_t>::err(rRes.error());
            reader = std::move(rRes).value();
            openVol = ref.volumeIndex;
        }
        if (ref.sourceFormat == ComicFormat::PDF) {
            if (auto* pdf = dynamic_cast<PdfDocumentReader*>(reader.get())) {
                auto fr = pdf->renderPage(ref.pageIndex, 0, 0, stopToken);
                if (!fr) return Result<std::uint32_t>::err(fr.error());
                maxWidth = (std::max)(maxWidth, fr.value().width);
            }
        } else {
            auto rawRes = reader->readPageBytes(ref.pageIndex, stopToken);
            if (!rawRes) return Result<std::uint32_t>::err(rawRes.error());
            if (auto w = probeImageWidthFromBytes(rawRes.value()); w.has_value()) {
                maxWidth = (std::max)(maxWidth, *w);
            }
        }
    }

    if (toLowerAscii(opts.scaleMode) == "max_width" && opts.scaleValue > 0 && maxWidth > 0) {
        maxWidth = (std::min)(maxWidth, static_cast<std::uint32_t>(opts.scaleValue));
    }
    return Result<std::uint32_t>::ok(maxWidth);
}

[[nodiscard]] std::string jsonEscapeString(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (unsigned char ch : input) {
        switch (ch) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[7]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(ch));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

struct OcrPageOutputItem {
    std::size_t pageNumber1Based{1};
    std::string headerLabel{};
    mavira::core::imaging::OcrResult ocr{};
};

[[nodiscard]] std::string resolveOcrFormat(const ToolExecutionOptions& opts) {
    const std::string ocrFmt = toLowerAscii(opts.ocrOutputFormat);
    if (ocrFmt == "md" || ocrFmt == "json" || ocrFmt == "txt") {
        return ocrFmt;
    }
    const std::string outFmt = toLowerAscii(opts.outputFormat);
    if (outFmt == "md" || outFmt == "json") {
        return outFmt;
    }
    return "txt";
}

[[nodiscard]] std::string formatOcrOutput(
    const std::string& sourceTitle,
    const std::vector<OcrPageOutputItem>& pages,
    std::string_view ocrFmt,
    bool includeHeaders) {
    std::ostringstream oss;
    if (ocrFmt == "json") {
        std::string resolvedLang;
        for (const auto& p : pages) {
            if (!p.ocr.languageTag.empty()) {
                resolvedLang = p.ocr.languageTag;
                break;
            }
        }
        oss << "{\n"
            << "  \"source\": \"" << jsonEscapeString(sourceTitle) << "\",\n"
            << "  \"language\": \"" << jsonEscapeString(resolvedLang) << "\",\n"
            << "  \"pages\": [\n";
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const auto& item = pages[i];
            oss << "    {\n"
                << "      \"page\": " << item.pageNumber1Based << ",\n";
            if (includeHeaders) {
                oss << "      \"header\": \"" << jsonEscapeString(item.headerLabel) << "\",\n";
            }
            oss << "      \"text\": \"" << jsonEscapeString(item.ocr.text) << "\",\n"
                << "      \"lines\": [";
            for (std::size_t li = 0; li < item.ocr.lines.size(); ++li) {
                if (li > 0) oss << ", ";
                oss << "\"" << jsonEscapeString(item.ocr.lines[li].text) << "\"";
            }
            oss << "]\n    }";
            if (i + 1 < pages.size()) oss << ",";
            oss << "\n";
        }
        oss << "  ]\n}\n";
        return oss.str();
    }

    if (ocrFmt == "md") {
        if (includeHeaders && !sourceTitle.empty()) {
            oss << "# " << sourceTitle << "\n\n";
        }
        for (const auto& item : pages) {
            if (includeHeaders) {
                oss << "## " << item.headerLabel << "\n\n";
            }
            oss << item.ocr.text << "\n\n";
        }
        return oss.str();
    }

    // "txt"
    for (const auto& item : pages) {
        if (includeHeaders) {
            oss << "=== " << item.headerLabel << " ===\n";
        }
        oss << item.ocr.text << "\n\n";
    }
    return oss.str();
}

[[nodiscard]] Result<std::vector<std::uint8_t>> encodeFrameWithOpts(
    const ImageFrame& frame,
    const ToolExecutionOptions& opts,
    ImageFormat sourceFormat) {
    const std::string fmt = toLowerAscii(opts.targetImageFormat);
    ImageFormat target = sourceFormat;
    if (fmt == "jpg" || fmt == "jpeg") {
        target = ImageFormat::JPEG;
    } else if (fmt == "png") {
        target = ImageFormat::PNG;
    } else if (fmt == "webp") {
        target = ImageFormat::WEBP;
    } else if (target != ImageFormat::PNG && target != ImageFormat::WEBP) {
        target = ImageFormat::JPEG;
    }

    switch (target) {
        case ImageFormat::PNG:
            return ImageDecoder::encodePng(frame, opts.pngCompression);
        case ImageFormat::WEBP:
            return ImageDecoder::encodeWebP(frame, static_cast<float>(opts.webpQuality), opts.webpLossless);
        case ImageFormat::JPEG:
        default:
            return ImageDecoder::encodeJpeg(frame, opts.jpgQuality);
    }
}

[[nodiscard]] Result<std::vector<std::uint8_t>> renderOrReadPageBytes(
    const PageSourceRef& ref,
    IArchiveReader* openReader,
    const ToolExecutionOptions& opts,
    bool processImages,
    std::stop_token stopToken,
    std::uint32_t normalizedWidth = 0) {
    if (stopToken.stop_requested()) {
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::Cancelled, "Operation cancelled");
    }

    auto finalizeFrame = [&](ImageFrame frame, ImageFormat srcFmt) -> Result<std::vector<std::uint8_t>> {
        if (processImages) {
            auto procRes = ImageDecoder::processFrame(frame, opts, stopToken);
            if (!procRes) return Result<std::vector<std::uint8_t>>::err(procRes.error());
            frame = std::move(procRes).value();
        }
        if (normalizedWidth > 0 && frame.width > 0 && frame.width != normalizedWidth) {
            const std::uint32_t newH = (std::max)(
                1u,
                static_cast<std::uint32_t>(std::lround(
                    static_cast<double>(frame.height) * static_cast<double>(normalizedWidth) /
                    static_cast<double>(frame.width))));
            frame = ImageDecoder::resizeExact(frame, normalizedWidth, newH);
        }
        if (!processImages && normalizedWidth > 0) {
            // Preserve source format when only normalizing width
            switch (srcFmt) {
                case ImageFormat::PNG:
                    return ImageDecoder::encodePng(frame, opts.pngCompression);
                case ImageFormat::WEBP:
                    return ImageDecoder::encodeWebP(frame, static_cast<float>(opts.webpQuality), opts.webpLossless);
                case ImageFormat::JPEG:
                default:
                    return ImageDecoder::encodeJpeg(frame, opts.jpgQuality);
            }
        }
        return encodeFrameWithOpts(frame, opts, srcFmt);
    };

    if (ref.isStandaloneImage) {
        auto rawRes = readFileBytes(ref.sourcePath);
        if (!rawRes) return rawRes;
        if (!processImages) {
            if (normalizedWidth == 0) {
                return rawRes;
            }
            if (auto w = probeImageWidthFromBytes(rawRes.value()); w.has_value() && *w == normalizedWidth) {
                return rawRes;
            }
        }
        const ImageFormat srcFmt = ImageDecoder::detectFormat(rawRes.value());
        auto decRes = ImageDecoder::decode(rawRes.value(), 0, 0, stopToken);
        if (!decRes) return Result<std::vector<std::uint8_t>>::err(decRes.error());
        return finalizeFrame(std::move(decRes).value(), srcFmt);
    }

    if (openReader == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::InvalidArgument, "Null archive reader");
    }

    if (ref.sourceFormat == ComicFormat::PDF) {
        if (auto* pdfReader = dynamic_cast<PdfDocumentReader*>(openReader)) {
            if (!processImages && normalizedWidth == 0) {
                if (auto rawRes = pdfReader->tryExtractRawPageImage(ref.pageIndex, stopToken); rawRes.isOk()) {
                    return rawRes;
                }
            }
            if (auto nativeRes = pdfReader->tryExtractNativePageBitmap(ref.pageIndex, stopToken); nativeRes.isOk()) {
                if (processImages || (normalizedWidth > 0 && nativeRes.value().width != normalizedWidth)) {
                    return finalizeFrame(std::move(nativeRes).value(), ImageFormat::JPEG);
                }
                return ImageDecoder::encodeJpeg(nativeRes.value(), opts.jpgQuality);
            }
            std::uint32_t targetW = 0;
            std::uint32_t targetH = 0;
            auto frameRes = pdfReader->renderPage(ref.pageIndex, 0, 0, stopToken);
            if (!frameRes) return Result<std::vector<std::uint8_t>>::err(frameRes.error());
            if (opts.pdfExtractDpi > 0 && opts.pdfExtractDpi != 150) {
                // Default PDFium render in PdfDocumentReader is ~150 DPI when 0,0; scale proportionally if needed.
                const double ratio = static_cast<double>(opts.pdfExtractDpi) / 150.0;
                targetW = static_cast<std::uint32_t>(
                    (std::max)(1L, std::lround(frameRes.value().width * ratio)));
                targetH = static_cast<std::uint32_t>(
                    (std::max)(1L, std::lround(frameRes.value().height * ratio)));
                frameRes = pdfReader->renderPage(ref.pageIndex, targetW, targetH, stopToken);
                if (!frameRes) return Result<std::vector<std::uint8_t>>::err(frameRes.error());
            }
            if (processImages || (normalizedWidth > 0 && frameRes.value().width != normalizedWidth)) {
                return finalizeFrame(std::move(frameRes).value(), ImageFormat::JPEG);
            }
            return ImageDecoder::encodeJpeg(frameRes.value(), opts.jpgQuality);
        }
    }

    auto rawRes = openReader->readPageBytes(ref.pageIndex, stopToken);
    if (!rawRes) return rawRes;
    if (!processImages) {
        if (normalizedWidth == 0) {
            return rawRes;
        }
        if (auto w = probeImageWidthFromBytes(rawRes.value()); w.has_value() && *w == normalizedWidth) {
            return rawRes;
        }
    }

    const ImageFormat srcFmt = ImageDecoder::detectFormat(rawRes.value());
    auto decRes = ImageDecoder::decode(rawRes.value(), 0, 0, stopToken);
    if (!decRes) return Result<std::vector<std::uint8_t>>::err(decRes.error());
    return finalizeFrame(std::move(decRes).value(), srcFmt);
}

[[nodiscard]] Result<void> writePagesSliceToCbz(
    const std::filesystem::path& outputPath,
    const std::vector<PageSourceRef>& slice,
    const std::vector<ChapterBookmark>& sliceBookmarks,
    const ToolExecutionOptions& opts,
    bool overwrite,
    bool processImages,
    std::uint32_t normalizedWidth,
    std::size_t& runningPagesDone,
    std::size_t totalPagesAll,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    std::error_code ec;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(FileManager::toWin32LongPath(outputPath.parent_path()), ec);
    }

    const std::filesystem::path tempPath(outputPath.native() + L".tmp");
    auto zipRes = openZipForWrite(tempPath);
    if (!zipRes) {
        return Result<void>::err(zipRes.error());
    }
    zip_t* outZip = zipRes.value();
    const auto [compMethod, compLevel] = resolveZipCompression(opts.compressionMode);

    // Keep encoded buffers alive until zip_close finishes; encode/render in parallel across workerThreads.
    std::vector<std::vector<std::uint8_t>> pageBuffers(slice.size());
    const std::size_t workers = static_cast<std::size_t>(
        std::clamp(opts.workerThreads, 1, static_cast<int>((std::min<std::size_t>)(slice.size(), 16))));

    if (workers <= 1) {
        std::size_t openVolIdx = static_cast<std::size_t>(-1);
        std::unique_ptr<IArchiveReader> activeReader;
        for (std::size_t i = 0; i < slice.size(); ++i) {
            if (stopToken.stop_requested()) {
                activeReader.reset();
                ::zip_discard(outZip);
                std::filesystem::remove(tempPath, ec);
                return Result<void>::err(ErrorCode::Cancelled, "Operation cancelled");
            }
            const auto& ref = slice[i];
            if (!ref.isStandaloneImage && (openVolIdx != ref.volumeIndex || activeReader == nullptr)) {
                activeReader.reset();
                auto r = ArchiveEngine::openArchive(ref.sourcePath);
                if (!r) {
                    ::zip_discard(outZip);
                    std::filesystem::remove(tempPath, ec);
                    return Result<void>::err(r.error());
                }
                activeReader = std::move(r).value();
                openVolIdx = ref.volumeIndex;
            }
            auto bytesRes = renderOrReadPageBytes(
                ref, activeReader.get(), opts, processImages, stopToken, normalizedWidth);
            if (!bytesRes) {
                activeReader.reset();
                ::zip_discard(outZip);
                std::filesystem::remove(tempPath, ec);
                return Result<void>::err(bytesRes.error());
            }
            pageBuffers[i] = std::move(bytesRes).value();
        }
        activeReader.reset();
    } else {
        std::atomic<std::size_t> nextIdx{0};
        std::atomic<std::size_t> completedCount{0};
        std::atomic<bool> abortWorkers{false};
        std::mutex errMutex;
        std::optional<Error> firstError;

        auto workerFn = [&]() {
            std::size_t openVolIdx = static_cast<std::size_t>(-1);
            std::unique_ptr<IArchiveReader> activeReader;
            for (;;) {
                if (abortWorkers.load(std::memory_order_relaxed) || stopToken.stop_requested()) {
                    return;
                }
                const std::size_t i = nextIdx.fetch_add(1, std::memory_order_relaxed);
                if (i >= slice.size()) {
                    return;
                }
                const auto& ref = slice[i];
                if (!ref.isStandaloneImage && (openVolIdx != ref.volumeIndex || activeReader == nullptr)) {
                    activeReader.reset();
                    auto r = ArchiveEngine::openArchive(ref.sourcePath);
                    if (!r) {
                        std::lock_guard<std::mutex> lock(errMutex);
                        if (!firstError.has_value()) firstError = r.error();
                        abortWorkers.store(true, std::memory_order_relaxed);
                        return;
                    }
                    activeReader = std::move(r).value();
                    openVolIdx = ref.volumeIndex;
                }
                auto bytesRes = renderOrReadPageBytes(
                    ref, activeReader.get(), opts, processImages, stopToken, normalizedWidth);
                if (!bytesRes) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (!firstError.has_value()) firstError = bytesRes.error();
                    abortWorkers.store(true, std::memory_order_relaxed);
                    return;
                }
                pageBuffers[i] = std::move(bytesRes).value();
                const std::size_t done = ++completedCount;
                if (progressCallback) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    MergeProgress prog;
                    prog.currentVolumeIndex = ref.volumeIndex;
                    prog.totalVolumes = 1;
                    prog.currentPageInVolume = done;
                    prog.pagesInCurrentVolume = slice.size();
                    prog.totalPagesProcessed = runningPagesDone + done;
                    prog.totalPagesEstimated = totalPagesAll;
                    prog.currentVolumeName = volumeDisplayTitle(ref.sourcePath);
                    progressCallback(prog);
                }
            }
        };

        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            pool.emplace_back(workerFn);
        }
        pool.clear();

        if (firstError.has_value()) {
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(*firstError);
        }
        if (stopToken.stop_requested()) {
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(ErrorCode::Cancelled, "Operation cancelled");
        }
    }

    for (std::size_t i = 0; i < slice.size(); ++i) {
        const auto& ref = slice[i];
        zip_source_t* src = ::zip_source_buffer(
            outZip, pageBuffers[i].data(), pageBuffers[i].size(), 0);
        if (src == nullptr) {
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(ErrorCode::IoError, "zip_source_buffer failed");
        }

        const zip_int64_t idx = ::zip_file_add(
            outZip, ref.entryName.c_str(), src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
        if (idx < 0) {
            ::zip_source_free(src);
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(ErrorCode::IoError, "zip_file_add failed");
        }
        (void)::zip_set_file_compression(outZip, static_cast<zip_uint64_t>(idx), compMethod, compLevel);

        ++runningPagesDone;
        if (workers <= 1 && progressCallback) {
            MergeProgress prog;
            prog.currentVolumeIndex = ref.volumeIndex;
            prog.totalVolumes = 1;
            prog.currentPageInVolume = i + 1;
            prog.pagesInCurrentVolume = slice.size();
            prog.totalPagesProcessed = runningPagesDone;
            prog.totalPagesEstimated = totalPagesAll;
            prog.currentVolumeName = volumeDisplayTitle(ref.sourcePath);
            progressCallback(prog);
        }
    }

    std::string comicInfoXml;
    if (opts.generateChapterBookmarks && !sliceBookmarks.empty()) {
        comicInfoXml = buildMergedComicInfoXml(outputPath, slice.size(), sliceBookmarks);
        zip_source_t* xmlSrc = ::zip_source_buffer(
            outZip, comicInfoXml.data(), comicInfoXml.size(), 0);
        if (xmlSrc != nullptr) {
            const zip_int64_t xmlIdx = ::zip_file_add(
                outZip, "ComicInfo.xml", xmlSrc, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
            if (xmlIdx >= 0) {
                (void)::zip_set_file_compression(outZip, static_cast<zip_uint64_t>(xmlIdx), compMethod, compLevel);
            } else {
                ::zip_source_free(xmlSrc);
            }
        }
    }

    const int closeRc = ::zip_close(outZip);
    pageBuffers.clear();
    if (closeRc != 0) {
        const std::string zipErr = ::zip_strerror(outZip);
        ::zip_discard(outZip);
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, "Failed finalizing CBZ: " + zipErr);
    }

    return atomicMoveFile(tempPath, outputPath, overwrite);
}

[[nodiscard]] Result<void> writePagesSliceToPdf(
    const std::filesystem::path& outputPath,
    const std::vector<PageSourceRef>& slice,
    const std::vector<ChapterBookmark>& sliceBookmarks,
    const ToolExecutionOptions& opts,
    bool overwrite,
    bool processImages,
    std::uint32_t normalizedWidth,
    std::size_t& runningPagesDone,
    std::size_t totalPagesAll,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken,
    bool* outAllRawPassThrough = nullptr) {
    std::error_code ec;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(FileManager::toWin32LongPath(outputPath.parent_path()), ec);
    }

    const std::filesystem::path tempPath(outputPath.native() + L".tmp");
    std::ofstream ofs(FileManager::toWin32LongPath(tempPath), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Result<void>::err(
            ErrorCode::IoError, "Failed opening temporary PDF: " + FileManager::wideToUtf8(tempPath.native()));
    }

    std::uint64_t byteOffset = 0;
    auto writeBytes = [&](const void* ptr, std::size_t len) -> bool {
        if (len == 0) return true;
        ofs.write(static_cast<const char*>(ptr), static_cast<std::streamsize>(len));
        byteOffset += static_cast<std::uint64_t>(len);
        return ofs.good();
    };
    auto writeStr = [&](std::string_view sv) -> bool {
        return writeBytes(sv.data(), sv.size());
    };

    const unsigned char headerBytes[] = {'%', 'P', 'D', 'F', '-', '1', '.', '4', '\n', '%', 0xE2, 0xE3, 0xCF, 0xD3, '\n'};
    if (!writeBytes(headerBytes, sizeof(headerBytes))) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, "Failed writing PDF header");
    }

    const bool includeOutlines = opts.generateChapterBookmarks && !sliceBookmarks.empty();
    const std::uint32_t catalogObjId = 1;
    const std::uint32_t pagesObjId = 2;
    const std::uint32_t outlinesObjId = includeOutlines ? 3u : 0u;
    std::uint32_t nextObjId = includeOutlines ? 4u : 3u;
    std::vector<std::uint64_t> objOffsets(nextObjId, 0);
    auto setObjOffset = [&](std::uint32_t id, std::uint64_t off) {
        if (id >= objOffsets.size()) objOffsets.resize(static_cast<std::size_t>(id) + 1, 0);
        objOffsets[id] = off;
    };

    std::vector<std::uint32_t> pageObjIds;
    pageObjIds.reserve(slice.size());

    const double dpiScale = (opts.pdfCreationDpi > 0) ? (72.0 / static_cast<double>(opts.pdfCreationDpi)) : 1.0;
    const std::string targetImgFmt = toLowerAscii(opts.targetImageFormat.empty() ? "original" : opts.targetImageFormat);
    const bool preferLosslessFlate =
        (!processImages && targetImgFmt == "original") ||
        (targetImgFmt == "png") ||
        (targetImgFmt == "webp" && opts.webpLossless);

    const std::size_t workers = static_cast<std::size_t>(
        std::clamp(opts.workerThreads, 1, static_cast<int>((std::min<std::size_t>)(slice.size(), 16))));
    const std::size_t maxInFlight = (std::max<std::size_t>)(8u, workers * 4u);

    std::vector<std::optional<PdfEncodedImage>> readySlots(slice.size());
    std::mutex queueMutex;
    std::condition_variable cvCanProduce;
    std::condition_variable cvCanWrite;
    std::atomic<std::size_t> nextEncodeIdx{0};
    std::size_t writtenCount = 0;
    std::atomic<bool> abortWorkers{false};
    std::optional<Error> firstError;

    auto workerFn = [&]() {
        std::size_t openVolIdx = static_cast<std::size_t>(-1);
        std::unique_ptr<IArchiveReader> activeReader;
        for (;;) {
            if (abortWorkers.load(std::memory_order_relaxed) || stopToken.stop_requested()) {
                return;
            }
            const std::size_t i = nextEncodeIdx.fetch_add(1, std::memory_order_relaxed);
            if (i >= slice.size()) {
                return;
            }
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cvCanProduce.wait(lock, [&]() {
                    return abortWorkers.load(std::memory_order_relaxed) ||
                           stopToken.stop_requested() ||
                           (i < writtenCount + maxInFlight);
                });
                if (abortWorkers.load(std::memory_order_relaxed) || stopToken.stop_requested()) {
                    return;
                }
            }

            const auto& ref = slice[i];
            if (!ref.isStandaloneImage && (openVolIdx != ref.volumeIndex || activeReader == nullptr)) {
                activeReader.reset();
                auto r = ArchiveEngine::openArchive(ref.sourcePath);
                if (!r) {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (!firstError.has_value()) firstError = r.error();
                    abortWorkers.store(true, std::memory_order_relaxed);
                    cvCanProduce.notify_all();
                    cvCanWrite.notify_all();
                    return;
                }
                activeReader = std::move(r).value();
                openVolIdx = ref.volumeIndex;
            }

            auto rawRes = renderOrReadPageBytes(
                ref, activeReader.get(), opts, processImages, stopToken, normalizedWidth);
            if (!rawRes) {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!firstError.has_value()) firstError = rawRes.error();
                abortWorkers.store(true, std::memory_order_relaxed);
                cvCanProduce.notify_all();
                cvCanWrite.notify_all();
                return;
            }

            auto imgRes = encodePdfImageStream(
                std::move(rawRes).value(), opts.jpgQuality, preferLosslessFlate, stopToken);
            if (!imgRes) {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!firstError.has_value()) firstError = imgRes.error();
                abortWorkers.store(true, std::memory_order_relaxed);
                cvCanProduce.notify_all();
                cvCanWrite.notify_all();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(queueMutex);
                readySlots[i] = std::move(imgRes).value();
            }
            cvCanWrite.notify_one();
        }
    };

    std::vector<std::jthread> pool;
    if (workers > 1) {
        pool.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            pool.emplace_back(workerFn);
        }
    }

    std::size_t seqOpenVolIdx = static_cast<std::size_t>(-1);
    std::unique_ptr<IArchiveReader> seqReader;
    std::optional<Error> writeError;
    bool allRawPassThrough = !processImages && (normalizedWidth == 0);

    for (std::size_t i = 0; i < slice.size(); ++i) {
        if (stopToken.stop_requested()) {
            abortWorkers.store(true, std::memory_order_relaxed);
            cvCanProduce.notify_all();
            writeError = Error{ErrorCode::Cancelled, "PDF generation cancelled"};
            break;
        }

        const auto& ref = slice[i];
        PdfEncodedImage encoded;
        if (workers <= 1) {
            if (!ref.isStandaloneImage && (seqOpenVolIdx != ref.volumeIndex || seqReader == nullptr)) {
                seqReader.reset();
                auto r = ArchiveEngine::openArchive(ref.sourcePath);
                if (!r) {
                    writeError = r.error();
                    break;
                }
                seqReader = std::move(r).value();
                seqOpenVolIdx = ref.volumeIndex;
            }
            auto rawRes = renderOrReadPageBytes(
                ref, seqReader.get(), opts, processImages, stopToken, normalizedWidth);
            if (!rawRes) {
                writeError = rawRes.error();
                break;
            }
            auto imgRes = encodePdfImageStream(
                std::move(rawRes).value(), opts.jpgQuality, preferLosslessFlate, stopToken);
            if (!imgRes) {
                writeError = imgRes.error();
                break;
            }
            encoded = std::move(imgRes).value();
        } else {
            std::unique_lock<std::mutex> lock(queueMutex);
            cvCanWrite.wait(lock, [&]() {
                return readySlots[i].has_value() ||
                       abortWorkers.load(std::memory_order_relaxed) ||
                       stopToken.stop_requested();
            });
            if (!readySlots[i].has_value()) {
                writeError = firstError.value_or(Error{ErrorCode::Cancelled, "PDF generation cancelled"});
                break;
            }
            encoded = std::move(*readySlots[i]);
            readySlots[i].reset();
            writtenCount = i + 1;
            cvCanProduce.notify_all();
        }

        const int imgW = encoded.width;
        const int imgH = encoded.height;
        if (ref.sourceFormat == ComicFormat::PDF || encoded.filterName != "/DCTDecode" || !encoded.decodeParms.empty()) {
            allRawPassThrough = false;
        }
        const double boxW = (std::max)(1.0, imgW * dpiScale);
        const double boxH = (std::max)(1.0, imgH * dpiScale);

        const std::uint32_t imgObjId = nextObjId++;
        const std::uint32_t contentObjId = nextObjId++;
        const std::uint32_t pageObjId = nextObjId++;
        pageObjIds.push_back(pageObjId);

        setObjOffset(imgObjId, byteOffset);
        std::ostringstream imgHdr;
        imgHdr << imgObjId << " 0 obj\n<< /Type /XObject /Subtype /Image /Width " << imgW
               << " /Height " << imgH << " /ColorSpace " << encoded.colorSpaceName
               << " /BitsPerComponent 8 /Filter " << encoded.filterName << ' '
               << encoded.decodeParms
               << "/Length " << encoded.streamBytes.size()
               << " >>\nstream\n";
        if (!writeStr(imgHdr.str()) ||
            !writeBytes(encoded.streamBytes.data(), encoded.streamBytes.size()) ||
            !writeStr("\nendstream\nendobj\n")) {
            abortWorkers.store(true, std::memory_order_relaxed);
            cvCanProduce.notify_all();
            writeError = Error{ErrorCode::IoError, "Failed writing PDF image object"};
            break;
        }
        encoded.streamBytes.clear();
        encoded.streamBytes.shrink_to_fit();

        std::ostringstream cs;
        cs << "q\n" << boxW << " 0 0 " << boxH << " 0 0 cm\n/Im0 Do\nQ\n";
        const std::string csStr = cs.str();
        setObjOffset(contentObjId, byteOffset);
        std::ostringstream cObj;
        cObj << contentObjId << " 0 obj\n<< /Length " << csStr.size() << " >>\nstream\n"
             << csStr << "endstream\nendobj\n";
        if (!writeStr(cObj.str())) {
            abortWorkers.store(true, std::memory_order_relaxed);
            cvCanProduce.notify_all();
            writeError = Error{ErrorCode::IoError, "Failed writing PDF content stream"};
            break;
        }

        setObjOffset(pageObjId, byteOffset);
        std::ostringstream pObj;
        pObj << pageObjId << " 0 obj\n<< /Type /Page /Parent " << pagesObjId << " 0 R "
             << "/MediaBox [0 0 " << boxW << ' ' << boxH << "] "
             << "/Resources << /XObject << /Im0 " << imgObjId << " 0 R >> >> "
             << "/Contents " << contentObjId << " 0 R >>\nendobj\n";
        if (!writeStr(pObj.str())) {
            abortWorkers.store(true, std::memory_order_relaxed);
            cvCanProduce.notify_all();
            writeError = Error{ErrorCode::IoError, "Failed writing PDF page object"};
            break;
        }

        ++runningPagesDone;
        if (progressCallback) {
            MergeProgress prog;
            prog.currentVolumeIndex = ref.volumeIndex;
            prog.totalVolumes = 1;
            prog.currentPageInVolume = i + 1;
            prog.pagesInCurrentVolume = slice.size();
            prog.totalPagesProcessed = runningPagesDone;
            prog.totalPagesEstimated = totalPagesAll;
            prog.currentVolumeName = volumeDisplayTitle(ref.sourcePath);
            progressCallback(prog);
        }
    }

    abortWorkers.store(true, std::memory_order_relaxed);
    cvCanProduce.notify_all();
    pool.clear();
    seqReader.reset();
    if (writeError.has_value()) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(*writeError);
    }

    if (includeOutlines) {
        std::vector<std::uint32_t> itemIds;
        for (std::size_t i = 0; i < sliceBookmarks.size(); ++i) itemIds.push_back(nextObjId++);
        for (std::size_t i = 0; i < sliceBookmarks.size(); ++i) {
            const auto& bm = sliceBookmarks[i];
            const std::size_t pIdx = (std::min)(static_cast<std::size_t>(bm.startPageIndex), pageObjIds.size() - 1);
            setObjOffset(itemIds[i], byteOffset);
            std::ostringstream io;
            io << itemIds[i] << " 0 obj\n<< /Title (" << pdfEscapeString(bm.title) << ") /Parent "
               << outlinesObjId << " 0 R ";
            if (i > 0) io << "/Prev " << itemIds[i - 1] << " 0 R ";
            if (i + 1 < itemIds.size()) io << "/Next " << itemIds[i + 1] << " 0 R ";
            io << "/Dest [" << pageObjIds[pIdx] << " 0 R /Fit] >>\nendobj\n";
            if (!writeStr(io.str())) {
                ofs.close();
                std::filesystem::remove(tempPath, ec);
                return Result<void>::err(ErrorCode::IoError, "Failed writing PDF outline item");
            }
        }
        setObjOffset(outlinesObjId, byteOffset);
        std::ostringstream oroot;
        oroot << outlinesObjId << " 0 obj\n<< /Type /Outlines /First " << itemIds.front()
              << " 0 R /Last " << itemIds.back() << " 0 R /Count " << itemIds.size() << " >>\nendobj\n";
        if (!writeStr(oroot.str())) {
            ofs.close();
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(ErrorCode::IoError, "Failed writing PDF outlines root");
        }
    }

    setObjOffset(pagesObjId, byteOffset);
    std::ostringstream pr;
    pr << pagesObjId << " 0 obj\n<< /Type /Pages /Kids [";
    for (std::size_t i = 0; i < pageObjIds.size(); ++i) {
        if (i > 0) pr << ' ';
        pr << pageObjIds[i] << " 0 R";
    }
    pr << "] /Count " << pageObjIds.size() << " >>\nendobj\n";
    if (!writeStr(pr.str())) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, "Failed writing PDF pages root");
    }

    setObjOffset(catalogObjId, byteOffset);
    std::ostringstream cat;
    cat << catalogObjId << " 0 obj\n<< /Type /Catalog /Pages " << pagesObjId << " 0 R";
    if (includeOutlines) cat << " /Outlines " << outlinesObjId << " 0 R /PageMode /UseOutlines";
    cat << " >>\nendobj\n";
    if (!writeStr(cat.str())) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, "Failed writing PDF catalog");
    }

    const std::uint64_t xrefOff = byteOffset;
    std::ostringstream xref;
    xref << "xref\n0 " << objOffsets.size() << "\n0000000000 65535 f \n";
    for (std::size_t i = 1; i < objOffsets.size(); ++i) {
        char entry[32];
        std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n",
                      static_cast<unsigned long long>(objOffsets[i]));
        xref << entry;
    }
    xref << "trailer\n<< /Size " << objOffsets.size() << " /Root " << catalogObjId << " 0 R >>\n"
         << "startxref\n" << xrefOff << "\n%%EOF\n";
    if (!writeStr(xref.str())) {
        ofs.close();
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, "Failed writing PDF xref");
    }

    ofs.flush();
    const bool good = ofs.good();
    ofs.close();
    if (!good) {
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, "Failed flushing PDF");
    }

    if (outAllRawPassThrough != nullptr) {
        *outAllRawPassThrough = allRawPassThrough;
    }
    return atomicMoveFile(tempPath, outputPath, overwrite);
}

[[nodiscard]] Result<void> writePagesSliceToLibArchive(
    const std::filesystem::path& outputPath,
    const std::vector<PageSourceRef>& slice,
    const std::vector<ChapterBookmark>& sliceBookmarks,
    const ToolExecutionOptions& opts,
    ComicFormat targetFormat,
    bool overwrite,
    bool processImages,
    std::uint32_t normalizedWidth,
    std::size_t& runningPagesDone,
    std::size_t totalPagesAll,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    std::error_code ec;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(FileManager::toWin32LongPath(outputPath.parent_path()), ec);
    }

    const std::filesystem::path tempPath(outputPath.native() + L".tmp");
    std::filesystem::remove(tempPath, ec);

    auto archRes = (targetFormat == ComicFormat::CBT)
        ? openCbtForWrite(tempPath)
        : openCb7ForWrite(tempPath, opts.compressionMode);
    if (!archRes) {
        return Result<void>::err(archRes.error());
    }
    struct archive* outArch = archRes.value();

    const std::size_t workers = static_cast<std::size_t>(
        std::clamp(opts.workerThreads, 1, static_cast<int>((std::min<std::size_t>)(slice.size(), 16))));
    const std::size_t maxInFlight = (std::max<std::size_t>)(8u, workers * 4u);

    std::vector<std::optional<std::vector<std::uint8_t>>> readySlots(slice.size());
    std::mutex queueMutex;
    std::condition_variable cvCanProduce;
    std::condition_variable cvCanWrite;
    std::atomic<std::size_t> nextEncodeIdx{0};
    std::size_t writtenCount = 0;
    std::atomic<bool> abortWorkers{false};
    std::optional<Error> firstError;

    auto workerFn = [&]() {
        std::size_t openVolIdx = static_cast<std::size_t>(-1);
        std::unique_ptr<IArchiveReader> activeReader;
        for (;;) {
            if (abortWorkers.load(std::memory_order_relaxed) || stopToken.stop_requested()) {
                return;
            }
            const std::size_t i = nextEncodeIdx.fetch_add(1, std::memory_order_relaxed);
            if (i >= slice.size()) {
                return;
            }
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cvCanProduce.wait(lock, [&]() {
                    return abortWorkers.load(std::memory_order_relaxed) ||
                           stopToken.stop_requested() ||
                           (i < writtenCount + maxInFlight);
                });
                if (abortWorkers.load(std::memory_order_relaxed) || stopToken.stop_requested()) {
                    return;
                }
            }

            const auto& ref = slice[i];
            if (!ref.isStandaloneImage && (openVolIdx != ref.volumeIndex || activeReader == nullptr)) {
                activeReader.reset();
                auto r = ArchiveEngine::openArchive(ref.sourcePath);
                if (!r) {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (!firstError.has_value()) firstError = r.error();
                    abortWorkers.store(true, std::memory_order_relaxed);
                    cvCanProduce.notify_all();
                    cvCanWrite.notify_all();
                    return;
                }
                activeReader = std::move(r).value();
                openVolIdx = ref.volumeIndex;
            }

            auto bytesRes = renderOrReadPageBytes(
                ref, activeReader.get(), opts, processImages, stopToken, normalizedWidth);
            if (!bytesRes) {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!firstError.has_value()) firstError = bytesRes.error();
                abortWorkers.store(true, std::memory_order_relaxed);
                cvCanProduce.notify_all();
                cvCanWrite.notify_all();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(queueMutex);
                readySlots[i] = std::move(bytesRes).value();
            }
            cvCanWrite.notify_one();
        }
    };

    std::vector<std::jthread> pool;
    if (workers > 1) {
        pool.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            pool.emplace_back(workerFn);
        }
    }

    std::size_t seqOpenVolIdx = static_cast<std::size_t>(-1);
    std::unique_ptr<IArchiveReader> seqReader;
    std::optional<Error> writeError;

    for (std::size_t i = 0; i < slice.size(); ++i) {
        if (stopToken.stop_requested()) {
            abortWorkers.store(true, std::memory_order_relaxed);
            cvCanProduce.notify_all();
            writeError = Error{ErrorCode::Cancelled, "CB7 generation cancelled"};
            break;
        }

        const auto& ref = slice[i];
        std::vector<std::uint8_t> buf;
        if (workers <= 1) {
            if (!ref.isStandaloneImage && (seqOpenVolIdx != ref.volumeIndex || seqReader == nullptr)) {
                seqReader.reset();
                auto r = ArchiveEngine::openArchive(ref.sourcePath);
                if (!r) {
                    writeError = r.error();
                    break;
                }
                seqReader = std::move(r).value();
                seqOpenVolIdx = ref.volumeIndex;
            }
            auto bytesRes = renderOrReadPageBytes(
                ref, seqReader.get(), opts, processImages, stopToken, normalizedWidth);
            if (!bytesRes) {
                writeError = bytesRes.error();
                break;
            }
            buf = std::move(bytesRes).value();
        } else {
            std::unique_lock<std::mutex> lock(queueMutex);
            cvCanWrite.wait(lock, [&]() {
                return readySlots[i].has_value() ||
                       abortWorkers.load(std::memory_order_relaxed) ||
                       stopToken.stop_requested();
            });
            if (!readySlots[i].has_value()) {
                writeError = firstError.value_or(Error{ErrorCode::Cancelled, "CB7 generation cancelled"});
                break;
            }
            buf = std::move(*readySlots[i]);
            readySlots[i].reset();
            writtenCount = i + 1;
            cvCanProduce.notify_all();
        }

        auto wRes = writeCb7Entry(outArch, ref.entryName, buf.data(), buf.size());
        if (!wRes) {
            abortWorkers.store(true, std::memory_order_relaxed);
            cvCanProduce.notify_all();
            writeError = wRes.error();
            break;
        }

        ++runningPagesDone;
        if (progressCallback) {
            MergeProgress prog;
            prog.currentVolumeIndex = ref.volumeIndex;
            prog.totalVolumes = 1;
            prog.currentPageInVolume = i + 1;
            prog.pagesInCurrentVolume = slice.size();
            prog.totalPagesProcessed = runningPagesDone;
            prog.totalPagesEstimated = totalPagesAll;
            prog.currentVolumeName = volumeDisplayTitle(ref.sourcePath);
            progressCallback(prog);
        }
    }

    abortWorkers.store(true, std::memory_order_relaxed);
    cvCanProduce.notify_all();
    pool.clear();
    seqReader.reset();

    if (writeError.has_value()) {
        ::archive_write_close(outArch);
        ::archive_write_free(outArch);
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(*writeError);
    }

    if (opts.generateChapterBookmarks && !sliceBookmarks.empty()) {
        const std::string xml = buildMergedComicInfoXml(outputPath, slice.size(), sliceBookmarks);
        auto xmlRes = writeCb7Entry(outArch, "ComicInfo.xml", xml.data(), xml.size());
        if (!xmlRes) {
            ::archive_write_close(outArch);
            ::archive_write_free(outArch);
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(xmlRes.error());
        }
    }

    const int closeRc = ::archive_write_close(outArch);
    const std::string closeErr = (closeRc != ARCHIVE_OK && ::archive_error_string(outArch))
        ? std::string(::archive_error_string(outArch))
        : std::string("archive_write_close failed");
    ::archive_write_free(outArch);
    if (closeRc != ARCHIVE_OK) {
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, closeErr);
    }

    return atomicMoveFile(tempPath, outputPath, overwrite);
}

[[nodiscard]] Result<void> writePagesSliceToEpub(
    const std::filesystem::path& outputPath,
    const std::vector<PageSourceRef>& slice,
    const std::vector<ChapterBookmark>& sliceBookmarks,
    const ToolExecutionOptions& opts,
    bool overwrite,
    bool processImages,
    std::uint32_t normalizedWidth,
    std::size_t& runningPagesDone,
    std::size_t totalPagesAll,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    std::error_code ec;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(FileManager::toWin32LongPath(outputPath.parent_path()), ec);
    }

    const std::filesystem::path tempPath(outputPath.native() + L".tmp");
    auto zipRes = openZipForWrite(tempPath);
    if (!zipRes) {
        return Result<void>::err(zipRes.error());
    }
    zip_t* outZip = zipRes.value();
    const auto [compMethod, compLevel] = resolveZipCompression(opts.compressionMode);

    std::vector<std::vector<std::uint8_t>> pageBuffers(slice.size());
    const std::size_t workers = static_cast<std::size_t>(
        std::clamp(opts.workerThreads, 1, static_cast<int>((std::min<std::size_t>)(slice.size(), 16))));

    if (workers <= 1) {
        std::size_t openVolIdx = static_cast<std::size_t>(-1);
        std::unique_ptr<IArchiveReader> activeReader;
        for (std::size_t i = 0; i < slice.size(); ++i) {
            if (stopToken.stop_requested()) {
                activeReader.reset();
                ::zip_discard(outZip);
                std::filesystem::remove(tempPath, ec);
                return Result<void>::err(ErrorCode::Cancelled, "Operation cancelled");
            }
            const auto& ref = slice[i];
            if (!ref.isStandaloneImage && (openVolIdx != ref.volumeIndex || activeReader == nullptr)) {
                activeReader.reset();
                auto r = ArchiveEngine::openArchive(ref.sourcePath);
                if (!r) {
                    ::zip_discard(outZip);
                    std::filesystem::remove(tempPath, ec);
                    return Result<void>::err(r.error());
                }
                activeReader = std::move(r).value();
                openVolIdx = ref.volumeIndex;
            }
            auto bytesRes = renderOrReadPageBytes(
                ref, activeReader.get(), opts, processImages, stopToken, normalizedWidth);
            if (!bytesRes) {
                activeReader.reset();
                ::zip_discard(outZip);
                std::filesystem::remove(tempPath, ec);
                return Result<void>::err(bytesRes.error());
            }
            pageBuffers[i] = std::move(bytesRes).value();
        }
        activeReader.reset();
    } else {
        std::atomic<std::size_t> nextIdx{0};
        std::atomic<std::size_t> completedCount{0};
        std::atomic<bool> abortWorkers{false};
        std::mutex errMutex;
        std::optional<Error> firstError;

        auto workerFn = [&]() {
            std::size_t openVolIdx = static_cast<std::size_t>(-1);
            std::unique_ptr<IArchiveReader> activeReader;
            for (;;) {
                if (abortWorkers.load(std::memory_order_relaxed) || stopToken.stop_requested()) {
                    return;
                }
                const std::size_t i = nextIdx.fetch_add(1, std::memory_order_relaxed);
                if (i >= slice.size()) {
                    return;
                }
                const auto& ref = slice[i];
                if (!ref.isStandaloneImage && (openVolIdx != ref.volumeIndex || activeReader == nullptr)) {
                    activeReader.reset();
                    auto r = ArchiveEngine::openArchive(ref.sourcePath);
                    if (!r) {
                        std::lock_guard<std::mutex> lock(errMutex);
                        if (!firstError.has_value()) firstError = r.error();
                        abortWorkers.store(true, std::memory_order_relaxed);
                        return;
                    }
                    activeReader = std::move(r).value();
                    openVolIdx = ref.volumeIndex;
                }
                auto bytesRes = renderOrReadPageBytes(
                    ref, activeReader.get(), opts, processImages, stopToken, normalizedWidth);
                if (!bytesRes) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (!firstError.has_value()) firstError = bytesRes.error();
                    abortWorkers.store(true, std::memory_order_relaxed);
                    return;
                }
                pageBuffers[i] = std::move(bytesRes).value();
                const std::size_t done = ++completedCount;
                if (progressCallback) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    MergeProgress prog;
                    prog.currentVolumeIndex = ref.volumeIndex;
                    prog.totalVolumes = 1;
                    prog.currentPageInVolume = done;
                    prog.pagesInCurrentVolume = slice.size();
                    prog.totalPagesProcessed = runningPagesDone + done;
                    prog.totalPagesEstimated = totalPagesAll;
                    prog.currentVolumeName = volumeDisplayTitle(ref.sourcePath);
                    progressCallback(prog);
                }
            }
        };

        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            pool.emplace_back(workerFn);
        }
        pool.clear();

        if (firstError.has_value()) {
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(*firstError);
        }
        if (stopToken.stop_requested()) {
            ::zip_discard(outZip);
            std::filesystem::remove(tempPath, ec);
            return Result<void>::err(ErrorCode::Cancelled, "Operation cancelled");
        }
    }

    auto abortEpub = [&](ErrorCode code, std::string msg) {
        ::zip_discard(outZip);
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(code, std::move(msg));
    };

    const std::string mimetypeStr = "application/epub+zip";
    const std::string containerXml = buildEpubContainerXml();
    if (auto r = addZipBufferEntry(
            outZip, "mimetype", mimetypeStr.data(), mimetypeStr.size(), ZIP_CM_STORE, 0, false);
        !r) {
        return abortEpub(r.errorCode(), r.error().message);
    }
    if (auto r = addZipBufferEntry(
            outZip, "META-INF/container.xml", containerXml.data(), containerXml.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortEpub(r.errorCode(), r.error().message);
    }

    const int digits = epubDigitsForCount(slice.size());
    std::vector<EpubPageMeta> pageMetas;
    pageMetas.reserve(slice.size());
    std::vector<std::string> pageXhtmls;
    pageXhtmls.reserve(slice.size());

    for (std::size_t i = 0; i < slice.size(); ++i) {
        const auto& ref = slice[i];
        const std::string extHint = FileManager::wideToUtf8(
            std::filesystem::path(FileManager::utf8ToWide(ref.entryName)).extension().native());
        EpubPageMeta meta;
        probeEpubImageInfo(pageBuffers[i], extHint, meta.width, meta.height, meta.mimeType);
        const std::string base = formatEpubPageBaseName(i + 1, digits);
        meta.imageHref = "images/" + base + (extHint.empty() ? ".jpg" : extHint);
        meta.imageEntryPath = "OEBPS/" + meta.imageHref;
        meta.xhtmlHref = base + ".xhtml";
        meta.xhtmlEntryPath = "OEBPS/" + meta.xhtmlHref;

        pageXhtmls.push_back(buildEpubPageXhtml(i + 1, meta));
        if (auto r = addZipBufferEntry(
                outZip,
                meta.imageEntryPath.c_str(),
                pageBuffers[i].data(),
                pageBuffers[i].size(),
                compMethod,
                compLevel);
            !r) {
            return abortEpub(r.errorCode(), r.error().message);
        }
        if (auto r = addZipBufferEntry(
                outZip,
                meta.xhtmlEntryPath.c_str(),
                pageXhtmls.back().data(),
                pageXhtmls.back().size(),
                ZIP_CM_DEFLATE,
                6);
            !r) {
            return abortEpub(r.errorCode(), r.error().message);
        }
        pageMetas.push_back(std::move(meta));

        ++runningPagesDone;
        if (workers <= 1 && progressCallback) {
            MergeProgress prog;
            prog.currentVolumeIndex = ref.volumeIndex;
            prog.totalVolumes = 1;
            prog.currentPageInVolume = i + 1;
            prog.pagesInCurrentVolume = slice.size();
            prog.totalPagesProcessed = runningPagesDone;
            prog.totalPagesEstimated = totalPagesAll;
            prog.currentVolumeName = volumeDisplayTitle(ref.sourcePath);
            progressCallback(prog);
        }
    }

    const std::string title = volumeDisplayTitle(outputPath);
    const std::vector<ChapterBookmark> emptyBms;
    const auto& effectiveBms = opts.generateChapterBookmarks ? sliceBookmarks : emptyBms;
    const std::string tocNcx = buildEpubTocNcx(title, pageMetas, effectiveBms);
    const std::string navXhtml = buildEpubNavXhtml(title, pageMetas, effectiveBms);
    const std::string contentOpf = buildEpubContentOpf(title, pageMetas);

    if (auto r = addZipBufferEntry(
            outZip, "OEBPS/toc.ncx", tocNcx.data(), tocNcx.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortEpub(r.errorCode(), r.error().message);
    }
    if (auto r = addZipBufferEntry(
            outZip, "OEBPS/nav.xhtml", navXhtml.data(), navXhtml.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortEpub(r.errorCode(), r.error().message);
    }
    if (auto r = addZipBufferEntry(
            outZip, "OEBPS/content.opf", contentOpf.data(), contentOpf.size(), ZIP_CM_DEFLATE, 6);
        !r) {
        return abortEpub(r.errorCode(), r.error().message);
    }

    const int closeRc = ::zip_close(outZip);
    pageBuffers.clear();
    pageXhtmls.clear();
    if (closeRc != 0) {
        const std::string zipErr = ::zip_strerror(outZip);
        ::zip_discard(outZip);
        std::filesystem::remove(tempPath, ec);
        return Result<void>::err(ErrorCode::IoError, "Failed finalizing EPUB: " + zipErr);
    }

    return atomicMoveFile(tempPath, outputPath, overwrite);
}

[[nodiscard]] Result<ToolJobResult> writeArchiveParts(
    std::string_view toolId,
    const std::vector<PageSourceRef>& allPages,
    const std::vector<ChapterBookmark>& allBookmarks,
    const std::filesystem::path& baseOutputDir,
    const std::string& baseStemUtf8,
    const ToolExecutionOptions& opts,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken,
    std::uint32_t normalizedWidth = 0,
    std::set<std::wstring>* claimedPaths = nullptr,
    std::mutex* claimMutex = nullptr,
    std::atomic<std::size_t>* sharedPagesDone = nullptr,
    std::size_t sharedTotalPages = 0,
    std::string_view itemLabelOverride = {}) {
    ToolJobResult result;
    result.toolId = std::string(toolId);
    result.chapterBookmarks = allBookmarks;

    MergeProgressCallback effectiveProgress;
    if (progressCallback) {
        if (sharedPagesDone != nullptr && sharedTotalPages > 0) {
            effectiveProgress = [&, itemLabel = std::string(itemLabelOverride)](const MergeProgress& p) {
                MergeProgress globalProg = p;
                globalProg.totalPagesProcessed = (std::min)(++(*sharedPagesDone), sharedTotalPages);
                globalProg.totalPagesEstimated = sharedTotalPages;
                if (!itemLabel.empty()) {
                    globalProg.currentVolumeName = itemLabel;
                }
                progressCallback(globalProg);
            };
        } else {
            effectiveProgress = progressCallback;
        }
    }

    const std::string outFmt = toLowerAscii(opts.outputFormat);
    const std::string ext =
        (outFmt == "pdf")  ? ".pdf" :
        (outFmt == "cb7")  ? ".cb7" :
        (outFmt == "cbt")  ? ".cbt" :
        (outFmt == "zip")  ? ".zip" :
        (outFmt == "epub") ? ".epub" :
                             ".cbz";
    const std::size_t parts = static_cast<std::size_t>(
        std::clamp(opts.splitIntoFiles, 1, static_cast<int>(allPages.size())));
    const bool processImages = needsImageProcessing(opts);
    const bool hasPdfSource = std::any_of(
        allPages.begin(), allPages.end(),
        [](const PageSourceRef& r) { return r.sourceFormat == ComicFormat::PDF; });
    bool allRawPassThrough = !processImages && (normalizedWidth == 0) && !hasPdfSource;

    std::size_t runningPagesDone = 0;
    for (std::size_t part = 0; part < parts; ++part) {
        if (stopToken.stop_requested()) {
            return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Operation cancelled");
        }
        const std::size_t startIdx = (part * allPages.size()) / parts;
        const std::size_t endIdx = ((part + 1) * allPages.size()) / parts;
        if (startIdx >= endIdx) continue;

        std::vector<PageSourceRef> slice(allPages.begin() + startIdx, allPages.begin() + endIdx);
        std::vector<ChapterBookmark> sliceBms;
        for (const auto& bm : allBookmarks) {
            if (bm.startPageIndex >= startIdx && bm.startPageIndex < endIdx) {
                sliceBms.push_back(ChapterBookmark{
                    static_cast<std::uint32_t>(bm.startPageIndex - startIdx),
                    bm.title});
            }
        }

        const std::string partName = (parts == 1)
            ? (baseStemUtf8 + ext)
            : (baseStemUtf8 + "_part" + std::to_string(part + 1) + ext);
        const std::filesystem::path candidate = baseOutputDir / FileManager::utf8ToWide(partName);

        bool overwrite = false;
        std::optional<std::filesystem::path> resolved;
        if (claimMutex != nullptr) {
            std::lock_guard<std::mutex> lock(*claimMutex);
            resolved = resolveIfExistsPath(candidate, opts.ifExistsPolicy, overwrite, claimedPaths);
        } else {
            resolved = resolveIfExistsPath(candidate, opts.ifExistsPolicy, overwrite, claimedPaths);
        }
        if (!resolved.has_value()) {
            ++result.skippedCount;
            continue;
        }

        if (outFmt == "pdf") {
            bool partRaw = false;
            auto r = writePagesSliceToPdf(
                *resolved, slice, sliceBms, opts, overwrite, processImages, normalizedWidth,
                runningPagesDone, allPages.size(), effectiveProgress, stopToken, &partRaw);
            if (!r) return Result<ToolJobResult>::err(r.error());
            allRawPassThrough = allRawPassThrough && partRaw;
        } else if (outFmt == "cb7") {
            auto r = writePagesSliceToLibArchive(
                *resolved, slice, sliceBms, opts, ComicFormat::CB7, overwrite, processImages, normalizedWidth,
                runningPagesDone, allPages.size(), effectiveProgress, stopToken);
            if (!r) return Result<ToolJobResult>::err(r.error());
        } else if (outFmt == "cbt") {
            auto r = writePagesSliceToLibArchive(
                *resolved, slice, sliceBms, opts, ComicFormat::CBT, overwrite, processImages, normalizedWidth,
                runningPagesDone, allPages.size(), effectiveProgress, stopToken);
            if (!r) return Result<ToolJobResult>::err(r.error());
        } else if (outFmt == "epub") {
            auto r = writePagesSliceToEpub(
                *resolved, slice, sliceBms, opts, overwrite, processImages, normalizedWidth,
                runningPagesDone, allPages.size(), effectiveProgress, stopToken);
            if (!r) return Result<ToolJobResult>::err(r.error());
        } else {
            auto r = writePagesSliceToCbz(
                *resolved, slice, sliceBms, opts, overwrite, processImages, normalizedWidth,
                runningPagesDone, allPages.size(), effectiveProgress, stopToken);
            if (!r) return Result<ToolJobResult>::err(r.error());
        }

        result.outputPaths.push_back(*resolved);
        result.totalPagesProcessed += slice.size();
    }

    notifyExplorerDirUpdated(baseOutputDir);
    result.totalItemsProcessed = result.outputPaths.size();
    result.usedRawPassThrough = allRawPassThrough && !result.outputPaths.empty();
    return Result<ToolJobResult>::ok(std::move(result));
}

} // namespace

Result<ToolJobResult> ComicMerger::runToolJob(
    std::string_view toolId,
    const mavira::core::ToolExecutionOptions& options,
    const MergeProgressCallback& progressCallback,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Tool job cancelled");
    }
    if (options.inputPaths.empty()) {
        return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No input paths provided");
    }

    // -------------------------------------------------------------------------
    // 1. convert_images
    // -------------------------------------------------------------------------
    if (toolId == "convert_images") {
        const auto images = discoverImageInputs(options);
        if (images.empty()) {
            return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No image files found");
        }

        ToolExecutionOptions effOpts = options;
        if (toLowerAscii(effOpts.targetImageFormat) == "original") {
            const std::string outFmt = toLowerAscii(effOpts.outputFormat);
            if (outFmt == "jpg" || outFmt == "jpeg" || outFmt == "png" || outFmt == "webp") {
                effOpts.targetImageFormat = outFmt;
            }
        }
        const bool reencode =
            ImageDecoder::requiresImageReencode(effOpts) || effOpts.stripImageMetadata;

        ToolJobResult result;
        result.toolId = std::string(toolId);
        result.usedRawPassThrough = !reencode;
        std::set<std::wstring> claimedPaths;
        std::set<std::filesystem::path> touchedDirs;

        for (std::size_t i = 0; i < images.size(); ++i) {
            if (stopToken.stop_requested()) {
                notifyExplorerForDirs(touchedDirs);
                return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Image conversion cancelled");
            }
            const auto& item = images[i];
            auto rawRes = readFileBytes(item.path);
            if (!rawRes) {
                notifyExplorerForDirs(touchedDirs);
                return Result<ToolJobResult>::err(rawRes.error());
            }
            if (rawRes.value().empty()) {
                ++result.skippedCount;
                continue;
            }

            const ImageFormat srcFmt = ImageDecoder::detectFormat(rawRes.value());
            std::vector<std::uint8_t> outBytes;
            if (!reencode) {
                outBytes = std::move(rawRes).value();
            } else {
                auto decRes = ImageDecoder::decode(rawRes.value(), 0, 0, stopToken);
                if (!decRes) {
                    notifyExplorerForDirs(touchedDirs);
                    return Result<ToolJobResult>::err(decRes.error());
                }
                auto encRes = ImageDecoder::processAndEncode(decRes.value(), effOpts, srcFmt, stopToken);
                if (!encRes) {
                    notifyExplorerForDirs(touchedDirs);
                    return Result<ToolJobResult>::err(encRes.error());
                }
                outBytes = std::move(encRes).value();
            }

            if (outBytes.empty()) {
                ++result.skippedCount;
                continue;
            }

            const ImageFormat outFmt = (imageFormatFromString(effOpts.targetImageFormat) != ImageFormat::Unknown)
                ? imageFormatFromString(effOpts.targetImageFormat)
                : srcFmt;
            std::string stem = FileManager::wideToUtf8(item.path.stem().native());
            stem = stem.empty()
                ? ("image_" + std::to_string(i + 1))
                : FileManager::sanitizeWin32Name(stem);

            const std::filesystem::path outDir = resolveBaseOutputDir(item.path, item.rootDir, effOpts);
            touchedDirs.insert(outDir);
            const std::string ext = reencode
                ? extForImageFormat(outFmt)
                : toLowerAscii(FileManager::wideToUtf8(item.path.extension().native()));
            const std::filesystem::path candidate =
                outDir / FileManager::utf8ToWide(stem + (ext.empty() ? extForImageFormat(outFmt) : ext));

            bool overwrite = false;
            auto resolved = resolveIfExistsPath(
                candidate, effOpts.ifExistsPolicy, overwrite, &claimedPaths);
            if (!resolved.has_value()) {
                ++result.skippedCount;
                continue;
            }

            auto wRes = writeFileBytesAtomic(
                *resolved, outBytes.data(), outBytes.size(), overwrite);
            if (!wRes) {
                notifyExplorerForDirs(touchedDirs);
                return Result<ToolJobResult>::err(wRes.error());
            }

            result.outputPaths.push_back(*resolved);
            ++result.totalItemsProcessed;
            ++result.totalPagesProcessed;

            if (progressCallback) {
                MergeProgress prog;
                prog.currentVolumeIndex = i;
                prog.totalVolumes = images.size();
                prog.currentPageInVolume = 1;
                prog.pagesInCurrentVolume = 1;
                prog.totalPagesProcessed = i + 1;
                prog.totalPagesEstimated = images.size();
                prog.currentVolumeName =
                    "[" + std::to_string(i + 1) + "/" + std::to_string(images.size()) + "] " +
                    FileManager::wideToUtf8(item.path.filename().native());
                progressCallback(prog);
            }
        }

        notifyExplorerForDirs(touchedDirs);
        return Result<ToolJobResult>::ok(std::move(result));
    }

    // -------------------------------------------------------------------------
    // 2. create_comic
    // -------------------------------------------------------------------------
    if (toolId == "create_comic") {
        const auto images = discoverImageInputs(options);
        if (images.empty()) {
            return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No image files found to package");
        }

        const std::string folderMode = toLowerAscii(options.createFolderMode);
        if (folderMode == "single_combined") {
            std::vector<PageSourceRef> pages;
            pages.reserve(images.size());
            for (std::size_t i = 0; i < images.size(); ++i) {
                PageSourceRef ref;
                ref.volumeIndex = 0;
                ref.pageIndex = i;
                ref.sourcePath = images[i].path;
                ref.isStandaloneImage = true;
                const std::string ext = resolveEntryExt(
                    FileManager::wideToUtf8(images[i].path.filename().native()),
                    ComicFormat::Unknown,
                    options);
                ref.entryName = formatPrefixedEntryName(1, i + 1, ext);
                pages.push_back(std::move(ref));
            }

            const std::filesystem::path firstInput =
                stripTrailingSeparator(std::filesystem::path(options.inputPaths.front()));
            const std::filesystem::path outDir =
                (toLowerAscii(options.outputFolderMode) == "custom" && !options.customOutputDir.empty())
                ? stripTrailingSeparator(std::filesystem::path(options.customOutputDir))
                : firstInput.parent_path();
            const std::string stem = volumeDisplayTitle(firstInput);
            return writeArchiveParts(toolId, pages, {}, outDir, stem, options, progressCallback, stopToken);
        }

        // Default: "per_subfolder" - group images by immediate parent folder in natural order.
        std::set<std::wstring> inputDirKeys;
        std::error_code ec;
        for (const auto& rawW : options.inputPaths) {
            if (rawW.empty()) continue;
            const std::filesystem::path p = stripTrailingSeparator(std::filesystem::path(rawW));
            if (std::filesystem::is_directory(FileManager::toWin32LongPath(p), ec) && !ec) {
                inputDirKeys.insert(toLowerWideKey(FileManager::toWin32LongPath(p)));
            }
        }

        struct FolderGroup {
            std::filesystem::path dir{};
            std::filesystem::path rootDir{};
            bool fromDirectoryInput{false};
            std::vector<DiscoveredInput> items{};
        };
        std::vector<FolderGroup> groups;
        std::map<std::wstring, std::size_t> groupIndexByKey;

        for (const auto& img : images) {
            const std::filesystem::path parentDir = stripTrailingSeparator(img.path.parent_path());
            const std::wstring key = toLowerWideKey(FileManager::toWin32LongPath(parentDir));
            auto it = groupIndexByKey.find(key);
            if (it == groupIndexByKey.end()) {
                const bool isDirGroup =
                    inputDirKeys.contains(key) ||
                    inputDirKeys.contains(toLowerWideKey(FileManager::toWin32LongPath(stripTrailingSeparator(img.rootDir))));
                groupIndexByKey.emplace(key, groups.size());
                groups.push_back(FolderGroup{parentDir, stripTrailingSeparator(img.rootDir), isDirGroup, {img}});
            } else {
                groups[it->second].items.push_back(img);
            }
        }

        ToolJobResult combined;
        combined.toolId = std::string(toolId);
        combined.usedRawPassThrough = true;
        std::set<std::wstring> claimedPaths;
        std::mutex claimMutex;
        std::atomic<std::size_t> sharedPagesDone{0};
        const std::size_t totalImagesAll = images.size();

        for (std::size_t grpIdx = 0; grpIdx < groups.size(); ++grpIdx) {
            const auto& grp = groups[grpIdx];
            if (stopToken.stop_requested()) {
                return Result<ToolJobResult>::err(ErrorCode::Cancelled, "CBZ creation cancelled");
            }
            std::vector<PageSourceRef> pages;
            pages.reserve(grp.items.size());
            for (std::size_t i = 0; i < grp.items.size(); ++i) {
                PageSourceRef ref;
                ref.volumeIndex = 0;
                ref.pageIndex = i;
                ref.sourcePath = grp.items[i].path;
                ref.isStandaloneImage = true;
                const std::string ext = resolveEntryExt(
                    FileManager::wideToUtf8(grp.items[i].path.filename().native()),
                    ComicFormat::Unknown,
                    options);
                ref.entryName = formatPrefixedEntryName(1, i + 1, ext);
                pages.push_back(std::move(ref));
            }

            std::filesystem::path outDir;
            if (toLowerAscii(options.outputFolderMode) == "custom" && !options.customOutputDir.empty()) {
                outDir = stripTrailingSeparator(std::filesystem::path(options.customOutputDir));
                if (options.keepSubfolderStructure && !grp.rootDir.empty() && grp.fromDirectoryInput) {
                    const std::filesystem::path rel =
                        std::filesystem::relative(grp.dir.parent_path(), grp.rootDir, ec);
                    if (!ec && !rel.empty() && rel != L"." && rel.native().rfind(L"..", 0) != 0) {
                        outDir /= rel;
                    }
                }
            } else {
                outDir = grp.fromDirectoryInput ? grp.dir.parent_path() : grp.dir;
            }

            const std::string stem = grp.fromDirectoryInput
                ? volumeDisplayTitle(grp.dir)
                : volumeDisplayTitle(grp.items.front().path);
            const std::string itemLabel =
                "[" + std::to_string(grpIdx + 1) + "/" + std::to_string(groups.size()) + "] " + stem;
            auto partRes = writeArchiveParts(
                toolId, pages, {}, outDir, stem, options, progressCallback, stopToken,
                0, &claimedPaths, &claimMutex, &sharedPagesDone, totalImagesAll, itemLabel);
            if (!partRes) return partRes;

            for (const auto& op : partRes.value().outputPaths) {
                combined.outputPaths.push_back(op);
            }
            combined.totalItemsProcessed += partRes.value().totalItemsProcessed;
            combined.totalPagesProcessed += partRes.value().totalPagesProcessed;
            combined.skippedCount += partRes.value().skippedCount;
            if (!partRes.value().usedRawPassThrough) {
                combined.usedRawPassThrough = false;
            }
        }
        if (combined.outputPaths.empty()) {
            combined.usedRawPassThrough = false;
        }
        return Result<ToolJobResult>::ok(std::move(combined));
    }

    // -------------------------------------------------------------------------
    // 3. merge_comics
    // -------------------------------------------------------------------------
    if (toolId == "merge_comics") {
        const auto inputs = discoverComicInputs(options, /*allowFolderAsVolume=*/true);
        if (inputs.empty()) {
            return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No comic inputs found to merge");
        }

        std::vector<PageSourceRef> allPages;
        std::vector<ChapterBookmark> bookmarks;
        for (std::size_t v = 0; v < inputs.size(); ++v) {
            if (stopToken.stop_requested()) {
                return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Merge cancelled");
            }
            const auto& p = inputs[v].path;
            const ComicFormat fmt = ArchiveEngine::detectFormat(p);
            auto rRes = ArchiveEngine::openArchive(p);
            if (!rRes) return Result<ToolJobResult>::err(rRes.error());
            const auto& reader = *rRes.value();
            const std::size_t count = reader.pageCount();
            if (count > 0) {
                bookmarks.push_back(ChapterBookmark{
                    static_cast<std::uint32_t>(allPages.size()),
                    volumeDisplayTitle(p)});
            }
            for (std::size_t page = 0; page < count; ++page) {
                PageSourceRef ref;
                ref.volumeIndex = v;
                ref.pageIndex = page;
                ref.sourcePath = p;
                ref.sourceFormat = fmt;
                ref.entryName = formatPrefixedEntryName(
                    v + 1, page + 1, resolveEntryExt(reader.pageName(page), fmt, options));
                allPages.push_back(std::move(ref));
            }
        }
        if (allPages.empty()) {
            return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No pages found across inputs");
        }

        std::uint32_t normalizedWidth = 0;
        if (options.mergeNormalizeWidth) {
            auto wRes = probeMaxPageWidth(allPages, options, stopToken);
            if (!wRes) return Result<ToolJobResult>::err(wRes.error());
            normalizedWidth = wRes.value();
        }

        const std::filesystem::path outDir = resolveBaseOutputDir(inputs.front().path, inputs.front().rootDir, options);
        const std::string stem = volumeDisplayTitle(inputs.front().path);
        return writeArchiveParts(
            toolId, allPages, bookmarks, outDir, stem, options, progressCallback, stopToken, normalizedWidth);
    }

    // -------------------------------------------------------------------------
    // 4. convert_comics
    // -------------------------------------------------------------------------
    if (toolId == "convert_comics") {
        const auto inputs = discoverComicInputs(options, /*allowFolderAsVolume=*/true);
        if (inputs.empty()) {
            return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No comic files found to convert");
        }

        // Pre-count total valid pages across all queued archives so totalPagesEstimated
        // stays monotonic and never jumps backward mid-batch.
        struct ConvertPlanItem {
            DiscoveredInput input{};
            ComicFormat format{ComicFormat::Unknown};
            std::size_t pageCount{0};
        };
        std::vector<ConvertPlanItem> plans;
        plans.reserve(inputs.size());
        std::size_t totalPagesAll = 0;
        for (const auto& item : inputs) {
            if (stopToken.stop_requested()) {
                return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Conversion cancelled");
            }
            const ComicFormat fmt = ArchiveEngine::detectFormat(item.path);
            auto rRes = ArchiveEngine::openArchive(item.path);
            if (!rRes) return Result<ToolJobResult>::err(rRes.error());
            const std::size_t count = rRes.value()->pageCount();
            if (count > 0) {
                totalPagesAll += count;
                plans.push_back(ConvertPlanItem{item, fmt, count});
            }
        }
        if (plans.empty() || totalPagesAll == 0) {
            return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No readable pages found to convert");
        }

        ToolJobResult combined;
        combined.toolId = std::string(toolId);
        combined.usedRawPassThrough = true;
        std::set<std::wstring> claimedPaths;
        std::atomic<std::size_t> sharedPagesDone{0};

        // Process queued archives strictly 1-by-1 sequentially so progress titles never ping-pong.
        // Multi-threading (options.workerThreads) is scoped strictly to pages within each active comic.
        for (std::size_t idx = 0; idx < plans.size(); ++idx) {
            if (stopToken.stop_requested()) {
                return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Conversion cancelled");
            }
            const auto& plan = plans[idx];
            const auto& item = plan.input;
            const ComicFormat fmt = plan.format;
            auto rRes = ArchiveEngine::openArchive(item.path);
            if (!rRes) return Result<ToolJobResult>::err(rRes.error());
            const auto& reader = *rRes.value();
            const std::size_t count = reader.pageCount();
            if (count == 0) {
                continue;
            }

            std::vector<PageSourceRef> pages;
            pages.reserve(count);
            for (std::size_t p = 0; p < count; ++p) {
                PageSourceRef ref;
                ref.volumeIndex = 0;
                ref.pageIndex = p;
                ref.sourcePath = item.path;
                ref.sourceFormat = fmt;
                ref.entryName = formatPrefixedEntryName(
                    1, p + 1, resolveEntryExt(reader.pageName(p), fmt, options));
                pages.push_back(std::move(ref));
            }

            const std::filesystem::path outDir = resolveBaseOutputDir(item.path, item.rootDir, options);
            const std::string stem = volumeDisplayTitle(item.path);
            const std::string itemLabel =
                "[" + std::to_string(idx + 1) + "/" + std::to_string(plans.size()) + "] " + stem;
            auto partRes = writeArchiveParts(
                toolId, pages, {}, outDir, stem, options, progressCallback, stopToken,
                0, &claimedPaths, nullptr, &sharedPagesDone, totalPagesAll, itemLabel);
            if (!partRes) return Result<ToolJobResult>::err(partRes.error());

            for (const auto& op : partRes.value().outputPaths) {
                combined.outputPaths.push_back(op);
            }
            combined.totalItemsProcessed += partRes.value().totalItemsProcessed;
            combined.totalPagesProcessed += partRes.value().totalPagesProcessed;
            combined.skippedCount += partRes.value().skippedCount;
            if (!partRes.value().usedRawPassThrough) {
                combined.usedRawPassThrough = false;
            }
        }

        if (combined.outputPaths.empty()) {
            combined.usedRawPassThrough = false;
        }
        return Result<ToolJobResult>::ok(std::move(combined));
    }

    // -------------------------------------------------------------------------
    // 5. extract_pages
    // -------------------------------------------------------------------------
    if (toolId == "extract_pages") {
        const auto inputs = discoverComicInputs(options, /*allowFolderAsVolume=*/false);
        if (inputs.empty()) {
            return Result<ToolJobResult>::err(ErrorCode::InvalidArgument, "No comic archives found to extract");
        }

        const bool processImages = ImageDecoder::requiresImageReencode(options);
        const bool convertImageFmt =
            toLowerAscii(options.targetImageFormat) != "original" && !options.targetImageFormat.empty();

        // Pre-plan all volumes to compute exact totalPagesAll before extraction starts.
        struct ExtractVolumePlan {
            DiscoveredInput input{};
            ComicFormat format{ComicFormat::Unknown};
            std::string comicStem{};
            std::vector<std::size_t> selectedPages{};
        };
        std::vector<ExtractVolumePlan> plans;
        plans.reserve(inputs.size());
        std::size_t totalPagesAll = 0;

        for (std::size_t v = 0; v < inputs.size(); ++v) {
            if (stopToken.stop_requested()) {
                return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Extraction cancelled");
            }
            const auto& item = inputs[v];
            const ComicFormat fmt = ArchiveEngine::detectFormat(item.path);
            auto rRes = ArchiveEngine::openArchive(item.path);
            if (!rRes) return Result<ToolJobResult>::err(rRes.error());
            const std::size_t total = rRes.value()->pageCount();

            auto rangeRes = parsePageRange(options.pageRange, total);
            if (!rangeRes) return Result<ToolJobResult>::err(rangeRes.error());
            auto selected = std::move(rangeRes).value();
            if (selected.empty()) {
                if (inputs.size() == 1) {
                    return Result<ToolJobResult>::err(
                        ErrorCode::InvalidArgument, "No pages matched the specified page range");
                }
                continue;
            }
            totalPagesAll += selected.size();
            plans.push_back(ExtractVolumePlan{
                item, fmt, volumeDisplayTitle(item.path), std::move(selected)});
        }

        if (plans.empty() || totalPagesAll == 0) {
            return Result<ToolJobResult>::err(
                ErrorCode::InvalidArgument, "No valid pages found to extract");
        }

        ToolJobResult result;
        result.toolId = std::string(toolId);
        std::set<std::filesystem::path> touchedDirs;
        std::size_t globalPagesProcessed = 0;

        for (std::size_t v = 0; v < plans.size(); ++v) {
            const auto& plan = plans[v];
            if (stopToken.stop_requested()) {
                notifyExplorerForDirs(touchedDirs);
                return Result<ToolJobResult>::err(ErrorCode::Cancelled, "Extraction cancelled");
            }

            auto rRes = ArchiveEngine::openArchive(plan.input.path);
            if (!rRes) {
                notifyExplorerForDirs(touchedDirs);
                return Result<ToolJobResult>::err(rRes.error());
            }
            auto& reader = *rRes.value();

            std::filesystem::path outFolder =
                resolveBaseOutputDir(plan.input.path, plan.input.rootDir, options);
            const std::filesystem::path destParent = outFolder;
            outFolder /= FileManager::utf8ToWide(plan.comicStem);

            std::error_code ec;
            const bool folderExistedBefore =
                std::filesystem::exists(FileManager::toWin32LongPath(outFolder), ec) && !ec;
            std::filesystem::create_directories(FileManager::toWin32LongPath(outFolder), ec);
            if (ec) {
                notifyExplorerForDirs(touchedDirs);
                return Result<ToolJobResult>::err(
                    ErrorCode::IoError, "Failed to create extraction subfolder: " + ec.message());
            }
            touchedDirs.insert(destParent);
            touchedDirs.insert(outFolder);

            int neededDigits = 1;
            for (std::size_t n = plan.selectedPages.size(); n >= 10; n /= 10) {
                ++neededDigits;
            }
            const int baseDigits = (options.pageNumberDigits > 0)
                ? ((options.pageNumberDigits == 3 && plan.selectedPages.size() >= 1000) ? 4 : options.pageNumberDigits)
                : 3;
            const int seqDigits = (std::max)(baseDigits, neededDigits);
            const int fallbackDigits = (std::max)(3, neededDigits);

            std::set<std::wstring> claimedPagePaths;
            std::mutex extractMutex;
            std::mutex readerMutex;
            std::atomic<std::size_t> pagesWrittenInFolder{0};
            std::atomic<std::size_t> volPagesDone{0};

            auto execRes = runParallelIndexed(
                plan.selectedPages.size(),
                options.workerThreads,
                stopToken,
                [&](std::size_t idx) -> Result<void> {
                    if (stopToken.stop_requested()) {
                        return Result<void>::err(ErrorCode::Cancelled, "Extraction cancelled");
                    }

                    const std::size_t pageIdx = plan.selectedPages[idx];
                    std::string rawEntryName;
                    Result<std::vector<std::uint8_t>> rawBytesRes =
                        Result<std::vector<std::uint8_t>>::err(ErrorCode::Unknown, "");
                    bool needPdfRender = (plan.format == ComicFormat::PDF);

                    if (!needPdfRender) {
                        std::lock_guard<std::mutex> lock(readerMutex);
                        rawEntryName = reader.pageName(pageIdx);
                        rawBytesRes = reader.readPageBytes(pageIdx, stopToken);
                    }

                    Result<std::vector<std::uint8_t>> bytesRes =
                        Result<std::vector<std::uint8_t>>::err(ErrorCode::Unknown, "");
                    if (needPdfRender) {
                        std::lock_guard<std::mutex> lock(readerMutex);
                        rawEntryName = reader.pageName(pageIdx);
                        PageSourceRef ref;
                        ref.volumeIndex = v;
                        ref.pageIndex = pageIdx;
                        ref.sourcePath = plan.input.path;
                        ref.sourceFormat = plan.format;
                        bytesRes = renderOrReadPageBytes(ref, &reader, options, processImages, stopToken);
                    } else if (!rawBytesRes) {
                        bytesRes = std::move(rawBytesRes);
                    } else if (!processImages) {
                        bytesRes = std::move(rawBytesRes);
                    } else {
                        const ImageFormat srcFmt = ImageDecoder::detectFormat(rawBytesRes.value());
                        auto decRes = ImageDecoder::decode(rawBytesRes.value(), 0, 0, stopToken);
                        if (!decRes) {
                            bytesRes = Result<std::vector<std::uint8_t>>::err(decRes.error());
                        } else {
                            auto procRes = ImageDecoder::processFrame(decRes.value(), options, stopToken);
                            if (!procRes) {
                                bytesRes = Result<std::vector<std::uint8_t>>::err(procRes.error());
                            } else {
                                bytesRes = encodeFrameWithOpts(procRes.value(), options, srcFmt);
                            }
                        }
                    }

                    if (!bytesRes) {
                        if (bytesRes.errorCode() == ErrorCode::Cancelled) {
                            return Result<void>::err(bytesRes.error());
                        }
                        {
                            std::lock_guard<std::mutex> lock(extractMutex);
                            ++result.skippedCount;
                        }
                        ++globalPagesProcessed;
                        return Result<void>::ok();
                    }

                    if (bytesRes.value().empty()) {
                        {
                            std::lock_guard<std::mutex> lock(extractMutex);
                            ++result.skippedCount;
                        }
                        ++globalPagesProcessed;
                        return Result<void>::ok();
                    }

                    const std::string ext = resolveEntryExt(rawEntryName, plan.format, options);

                    std::filesystem::path pageFileRel;
                    if (options.pageNumberDigits <= 0) {
                        std::filesystem::path internalName =
                            std::filesystem::path(FileManager::utf8ToWide(rawEntryName)).filename();
                        if (internalName.empty() || plan.format == ComicFormat::PDF) {
                            pageFileRel = FileManager::utf8ToWide(
                                formatSequentialPageName(idx + 1, fallbackDigits, ext));
                        } else {
                            if (convertImageFmt || internalName.extension().empty()) {
                                internalName.replace_extension(FileManager::utf8ToWide(ext));
                            }
                            const std::string cleanStem = FileManager::sanitizeWin32Name(
                                FileManager::wideToUtf8(internalName.stem().native()));
                            pageFileRel = FileManager::utf8ToWide(cleanStem) + internalName.extension().native();
                        }
                    } else {
                        pageFileRel = FileManager::utf8ToWide(
                            formatSequentialPageName(idx + 1, seqDigits, ext));
                    }

                    const std::filesystem::path candidate = outFolder / pageFileRel;
                    bool overwrite = false;
                    std::optional<std::filesystem::path> resolved;
                    {
                        std::lock_guard<std::mutex> lock(extractMutex);
                        resolved = resolveIfExistsPath(
                            candidate, options.ifExistsPolicy, overwrite, &claimedPagePaths);
                        if (!resolved.has_value()) {
                            ++result.skippedCount;
                            ++globalPagesProcessed;
                            return Result<void>::ok();
                        }
                    }

                    auto wRes = writeFileBytesAtomic(
                        *resolved, bytesRes.value().data(), bytesRes.value().size(), overwrite);
                    if (!wRes) {
                        return Result<void>::err(wRes.error());
                    }

                    {
                        std::lock_guard<std::mutex> lock(extractMutex);
                        result.outputPaths.push_back(*resolved);
                        ++result.totalPagesProcessed;
                    }
                    ++pagesWrittenInFolder;
                    const std::size_t doneInVol = ++volPagesDone;
                    const std::size_t gDone = (std::min)(++globalPagesProcessed, totalPagesAll);

                    if (progressCallback) {
                        std::lock_guard<std::mutex> lock(extractMutex);
                        MergeProgress prog;
                        prog.currentVolumeIndex = v;
                        prog.totalVolumes = plans.size();
                        prog.currentPageInVolume = doneInVol;
                        prog.pagesInCurrentVolume = plan.selectedPages.size();
                        prog.totalPagesProcessed = gDone;
                        prog.totalPagesEstimated = totalPagesAll;
                        prog.currentVolumeName =
                            "[" + std::to_string(v + 1) + "/" + std::to_string(plans.size()) + "] " +
                            plan.comicStem;
                        progressCallback(prog);
                    }
                    return Result<void>::ok();
                });

            if (!execRes) {
                if (!folderExistedBefore && pagesWrittenInFolder.load() == 0) {
                    std::filesystem::remove_all(FileManager::toWin32LongPath(outFolder), ec);
                }
                notifyExplorerForDirs(touchedDirs);
                return Result<ToolJobResult>::err(execRes.error());
            }

            if (!folderExistedBefore && pagesWrittenInFolder.load() == 0) {
                std::filesystem::remove_all(FileManager::toWin32LongPath(outFolder), ec);
            } else {
                ++result.totalItemsProcessed;
            }
        }

        notifyExplorerForDirs(touchedDirs);
        const bool hasPdfInput = std::any_of(
            plans.begin(), plans.end(),
            [](const ExtractVolumePlan& p) { return p.format == ComicFormat::PDF; });
        result.usedRawPassThrough = !processImages && !hasPdfInput && !result.outputPaths.empty();
        return Result<ToolJobResult>::ok(std::move(result));
    }

    // -------------------------------------------------------------------------
    // 6. extract_ocr
    // -------------------------------------------------------------------------
    if (toolId == "extract_ocr") {
        const auto comics = discoverComicInputs(options, /*allowFolderAsVolume=*/false);
        const bool processImages = needsImageProcessing(options);
        const std::string ocrFmt = resolveOcrFormat(options);
        const std::string ocrExt =
            (ocrFmt == "md") ? ".md" : ((ocrFmt == "json") ? ".json" : ".txt");
        ToolJobResult result;
        result.toolId = std::string(toolId);

        if (!comics.empty()) {
            std::size_t totalPagesAll = 0;
            std::vector<std::vector<std::size_t>> selectedPerComic(comics.size());
            for (std::size_t v = 0; v < comics.size(); ++v) {
                auto rRes = ArchiveEngine::openArchive(comics[v].path);
                if (!rRes) return Result<ToolJobResult>::err(rRes.error());
                auto rangeRes = parsePageRange(options.pageRange, rRes.value()->pageCount());
                if (!rangeRes) return Result<ToolJobResult>::err(rangeRes.error());
                selectedPerComic[v] = std::move(rangeRes).value();
                totalPagesAll += selectedPerComic[v].size();
            }

            std::set<std::wstring> claimedPaths;
            std::set<std::filesystem::path> touchedDirs;
            std::atomic<std::size_t> globalDone{0};

            for (std::size_t v = 0; v < comics.size(); ++v) {
                const auto& item = comics[v];
                const ComicFormat fmt = ArchiveEngine::detectFormat(item.path);
                auto rRes = ArchiveEngine::openArchive(item.path);
                if (!rRes) {
                    notifyExplorerForDirs(touchedDirs);
                    return Result<ToolJobResult>::err(rRes.error());
                }
                auto& reader = *rRes.value();
                const auto& selected = selectedPerComic[v];
                if (selected.empty()) {
                    if (comics.size() == 1) {
                        return Result<ToolJobResult>::err(
                            ErrorCode::InvalidArgument, "No pages matched the specified page range");
                    }
                    continue;
                }

                std::vector<OcrPageOutputItem> pageOutputs(selected.size());
                std::mutex readerMutex;
                std::atomic<std::size_t> doneCount{0};
                const std::string comicStem = volumeDisplayTitle(item.path);

                auto execRes = runParallelIndexed(
                    selected.size(),
                    options.workerThreads,
                    stopToken,
                    [&](std::size_t k) -> Result<void> {
                        const std::size_t p = selected[k];
                        ImageFrame frame;
                        {
                            std::lock_guard<std::mutex> lock(readerMutex);
                            if (fmt == ComicFormat::PDF) {
                                if (auto* pdf = dynamic_cast<PdfDocumentReader*>(&reader)) {
                                    auto fr = pdf->renderPage(p, 0, 0, stopToken);
                                    if (!fr) return Result<void>::err(fr.error());
                                    frame = std::move(fr).value();
                                }
                            } else {
                                auto raw = reader.readPageBytes(p, stopToken);
                                if (!raw) return Result<void>::err(raw.error());
                                auto dec = ImageDecoder::decode(raw.value(), 0, 0, stopToken);
                                if (!dec) return Result<void>::err(dec.error());
                                frame = std::move(dec).value();
                            }
                        }
                        if (processImages) {
                            auto proc = ImageDecoder::processFrame(frame, options, stopToken);
                            if (!proc) return Result<void>::err(proc.error());
                            frame = std::move(proc).value();
                        }
                        auto ocrRes = NativeOcrEngine::recognizeFrame(
                            frame,
                            options.ocrLanguage,
                            options.ocrUpscaleFactor,
                            options.ocrFilterNoiseSymbols,
                            options.ocrNormalizeAllCaps,
                            stopToken);
                        if (!ocrRes) return Result<void>::err(ocrRes.error());

                        pageOutputs[k].pageNumber1Based = p + 1;
                        pageOutputs[k].headerLabel = "Page " + std::to_string(p + 1);
                        pageOutputs[k].ocr = std::move(ocrRes).value();

                        const std::size_t done = ++doneCount;
                        const std::size_t gDone = (std::min)(++globalDone, totalPagesAll);
                        if (progressCallback) {
                            MergeProgress prog;
                            prog.currentVolumeIndex = v;
                            prog.totalVolumes = comics.size();
                            prog.currentPageInVolume = done;
                            prog.pagesInCurrentVolume = selected.size();
                            prog.totalPagesProcessed = gDone;
                            prog.totalPagesEstimated = totalPagesAll;
                            prog.currentVolumeName =
                                "[" + std::to_string(v + 1) + "/" + std::to_string(comics.size()) + "] " +
                                comicStem;
                            progressCallback(prog);
                        }
                        return Result<void>::ok();
                    });
                if (!execRes) {
                    notifyExplorerForDirs(touchedDirs);
                    return Result<ToolJobResult>::err(execRes.error());
                }

                const std::string text = formatOcrOutput(
                    comicStem, pageOutputs, ocrFmt, options.ocrIncludePageHeaders);

                const std::filesystem::path outDir = resolveBaseOutputDir(item.path, item.rootDir, options);
                touchedDirs.insert(outDir);
                const std::filesystem::path candidate =
                    outDir / FileManager::utf8ToWide(comicStem + ocrExt);
                bool overwrite = false;
                auto resolved = resolveIfExistsPath(
                    candidate, options.ifExistsPolicy, overwrite, &claimedPaths);
                if (!resolved.has_value()) {
                    ++result.skippedCount;
                    continue;
                }
                auto wRes = writeFileBytesAtomic(*resolved, text.data(), text.size(), overwrite);
                if (!wRes) {
                    notifyExplorerForDirs(touchedDirs);
                    return Result<ToolJobResult>::err(wRes.error());
                }

                result.outputPaths.push_back(*resolved);
                ++result.totalItemsProcessed;
                result.totalPagesProcessed += selected.size();
            }
            notifyExplorerForDirs(touchedDirs);
            return Result<ToolJobResult>::ok(std::move(result));
        }

        const auto images = discoverImageInputs(options);
        if (images.empty()) {
            return Result<ToolJobResult>::err(
                ErrorCode::InvalidArgument, "No comic or image files found for OCR");
        }

        auto rangeRes = parsePageRange(options.pageRange, images.size());
        if (!rangeRes) return Result<ToolJobResult>::err(rangeRes.error());
        const auto& selected = rangeRes.value();
        if (selected.empty()) {
            return Result<ToolJobResult>::err(
                ErrorCode::InvalidArgument, "No images matched the specified page range");
        }

        std::vector<OcrPageOutputItem> imgOutputs(selected.size());
        std::atomic<std::size_t> doneCount{0};
        auto execRes = runParallelIndexed(
            selected.size(),
            options.workerThreads,
            stopToken,
            [&](std::size_t k) -> Result<void> {
                const std::size_t idx = selected[k];
                auto rawRes = readFileBytes(images[idx].path);
                if (!rawRes) return Result<void>::err(rawRes.error());
                auto decRes = ImageDecoder::decode(rawRes.value(), 0, 0, stopToken);
                if (!decRes) return Result<void>::err(decRes.error());
                ImageFrame frame = std::move(decRes).value();
                if (processImages) {
                    auto proc = ImageDecoder::processFrame(frame, options, stopToken);
                    if (!proc) return Result<void>::err(proc.error());
                    frame = std::move(proc).value();
                }
                auto ocrRes = NativeOcrEngine::recognizeFrame(
                    frame,
                    options.ocrLanguage,
                    options.ocrUpscaleFactor,
                    options.ocrFilterNoiseSymbols,
                    options.ocrNormalizeAllCaps,
                    stopToken);
                if (!ocrRes) return Result<void>::err(ocrRes.error());

                imgOutputs[k].pageNumber1Based = idx + 1;
                imgOutputs[k].headerLabel = FileManager::wideToUtf8(images[idx].path.filename().native());
                imgOutputs[k].ocr = std::move(ocrRes).value();
                ++doneCount;
                return Result<void>::ok();
            });
        if (!execRes) return Result<ToolJobResult>::err(execRes.error());

        const std::filesystem::path firstInput =
            stripTrailingSeparator(std::filesystem::path(options.inputPaths.front()));
        std::error_code ec;
        const bool singleDirInput =
            options.inputPaths.size() == 1 &&
            std::filesystem::is_directory(FileManager::toWin32LongPath(firstInput), ec) && !ec;
        const std::string stem = singleDirInput
            ? volumeDisplayTitle(firstInput)
            : volumeDisplayTitle(images[selected.front()].path);
        const std::string text = formatOcrOutput(
            stem, imgOutputs, ocrFmt, options.ocrIncludePageHeaders);
        const std::filesystem::path outDir =
            resolveBaseOutputDir(images[selected.front()].path, images[selected.front()].rootDir, options);
        const std::filesystem::path candidate =
            outDir / FileManager::utf8ToWide(stem + ocrExt);
        bool overwrite = false;
        auto resolved = resolveIfExistsPath(candidate, options.ifExistsPolicy, overwrite);
        if (!resolved.has_value()) {
            ++result.skippedCount;
        } else {
            auto wRes = writeFileBytesAtomic(*resolved, text.data(), text.size(), overwrite);
            if (!wRes) return Result<ToolJobResult>::err(wRes.error());
            result.outputPaths.push_back(*resolved);
            ++result.totalItemsProcessed;
            result.totalPagesProcessed = selected.size();
        }
        return Result<ToolJobResult>::ok(std::move(result));
    }

    return Result<ToolJobResult>::err(
        ErrorCode::InvalidArgument, "Unsupported toolId: " + std::string(toolId));
}

} // namespace mavira::core::archive
