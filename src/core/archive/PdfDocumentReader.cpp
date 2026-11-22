#include "core/archive/PdfDocumentReader.hpp"

#include "data/fs/FileManager.hpp"

#include <Windows.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace mavira::core::archive {

namespace {

using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;
using mavira::data::fs::FileManager;

constexpr double kDefaultRenderScale = 150.0 / 72.0; // 150 DPI relative to PDF 72 pt/in
constexpr std::uint32_t kMaxRenderDimension = 8192;

void ensurePdfiumInitializedLocked() {
    static bool initialized = false;
    if (!initialized) {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;
        config.m_v8EmbedderSlot = 0;
        ::FPDF_InitLibraryWithConfig(&config);
        initialized = true;
    }
}

[[nodiscard]] std::string readPdfMetaTagLocked(FPDF_DOCUMENT doc, const char* tag) {
    if (doc == nullptr || tag == nullptr) {
        return {};
    }
    const unsigned long byteLen = ::FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (byteLen <= 2) {
        return {};
    }

    const std::size_t wcharCount = static_cast<std::size_t>(byteLen) / sizeof(wchar_t);
    std::wstring wideBuf(wcharCount, L'\0');
    const unsigned long written = ::FPDF_GetMetaText(doc, tag, wideBuf.data(), byteLen);
    if (written <= 2) {
        return {};
    }

    // Strip trailing UTF-16LE null terminator(s).
    while (!wideBuf.empty() && wideBuf.back() == L'\0') {
        wideBuf.pop_back();
    }
    return FileManager::wideToUtf8(wideBuf);
}

[[nodiscard]] ErrorCode mapPdfiumError(unsigned long fpdfErr) noexcept {
    switch (fpdfErr) {
        case FPDF_ERR_FILE:
            return ErrorCode::IoError;
        case FPDF_ERR_FORMAT:
            return ErrorCode::CorruptArchive;
        case FPDF_ERR_PASSWORD:
        case FPDF_ERR_SECURITY:
            return ErrorCode::PermissionDenied;
        case FPDF_ERR_PAGE:
            return ErrorCode::InvalidArgument;
        default:
            return ErrorCode::DecodeFailed;
    }
}

void writeLe16(std::uint8_t* dst, std::uint16_t val) noexcept {
    dst[0] = static_cast<std::uint8_t>(val & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((val >> 8) & 0xFFu);
}

void writeLe32(std::uint8_t* dst, std::uint32_t val) noexcept {
    dst[0] = static_cast<std::uint8_t>(val & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((val >> 8) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((val >> 16) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>((val >> 24) & 0xFFu);
}

/// Encodes an RGBA8888 ImageFrame into an uncompressed top-down 32-bit BGRA BMP buffer.
[[nodiscard]] std::vector<std::uint8_t> encodeRgbaToBmp(const ImageFrame& frame) {
    constexpr std::uint32_t kFileHeaderSize = 14;
    constexpr std::uint32_t kInfoHeaderSize = 40;
    constexpr std::uint32_t kHeaderSize = kFileHeaderSize + kInfoHeaderSize;

    const std::size_t pixelDataSize =
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4u;
    std::vector<std::uint8_t> out(kHeaderSize + pixelDataSize, 0);

    // BITMAPFILEHEADER
    out[0] = 'B';
    out[1] = 'M';
    writeLe32(out.data() + 2, static_cast<std::uint32_t>(out.size()));
    writeLe32(out.data() + 10, kHeaderSize);

    // BITMAPINFOHEADER (negative height = top-down DIB)
    writeLe32(out.data() + 14, kInfoHeaderSize);
    writeLe32(out.data() + 18, frame.width);
    writeLe32(out.data() + 22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(frame.height)));
    writeLe16(out.data() + 26, 1);  // planes
    writeLe16(out.data() + 28, 32); // bpp
    writeLe32(out.data() + 30, 0);  // BI_RGB
    writeLe32(out.data() + 34, static_cast<std::uint32_t>(pixelDataSize));

    // Convert RGBA -> BGRA
    const std::uint8_t* src = frame.pixels.data();
    std::uint8_t* dst = out.data() + kHeaderSize;
    const std::size_t pixelCount =
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2]; // B
        dst[i * 4 + 1] = src[i * 4 + 1]; // G
        dst[i * 4 + 2] = src[i * 4 + 0]; // R
        dst[i * 4 + 3] = src[i * 4 + 3]; // A
    }

    return out;
}

} // namespace

struct PdfDocumentReader::StreamContext {
    HANDLE fileHandle{INVALID_HANDLE_VALUE};
    std::uint64_t fileSize{0};
    FPDF_FILEACCESS fileAccess{};

    ~StreamContext() {
        closeHandle();
    }

    void closeHandle() noexcept {
        if (fileHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(fileHandle);
            fileHandle = INVALID_HANDLE_VALUE;
        }
        fileSize = 0;
    }

    /// 64-bit Win32 random-access read using `SetFilePointerEx` (`LARGE_INTEGER`) + `ReadFile`.
    [[nodiscard]] bool readAt64(
        std::uint64_t offset,
        unsigned char* dst,
        std::uint32_t size) const noexcept {
        if (fileHandle == INVALID_HANDLE_VALUE || dst == nullptr || size == 0) {
            return false;
        }
        if (offset > fileSize || size > fileSize - offset) {
            return false;
        }

        LARGE_INTEGER seekPos{};
        seekPos.QuadPart = static_cast<LONGLONG>(offset);
        if (!::SetFilePointerEx(fileHandle, seekPos, nullptr, FILE_BEGIN)) {
            return false;
        }

        std::uint32_t totalRead = 0;
        while (totalRead < size) {
            DWORD chunkRead = 0;
            const DWORD toRead = static_cast<DWORD>(size - totalRead);
            if (!::ReadFile(fileHandle, dst + totalRead, toRead, &chunkRead, nullptr)) {
                return false;
            }
            if (chunkRead == 0) {
                return false;
            }
            totalRead += static_cast<std::uint32_t>(chunkRead);
        }
        return true;
    }

    /// `FPDF_FILEACCESS::m_GetBlock` callback (FPDF_CUSTOM_READ).
    static int customReadCallback(
        void* param,
        unsigned long position,
        unsigned char* pBuf,
        unsigned long size) {
        const auto* ctx = static_cast<const StreamContext*>(param);
        if (ctx == nullptr) {
            return 0;
        }
        return ctx->readAt64(
                   static_cast<std::uint64_t>(position),
                   pBuf,
                   static_cast<std::uint32_t>(size))
            ? 1
            : 0;
    }
};

std::mutex& PdfDocumentReader::pdfiumMutex() noexcept {
    static std::mutex s_mutex;
    return s_mutex;
}

PdfDocumentReader::PdfDocumentReader() = default;

PdfDocumentReader::~PdfDocumentReader() {
    close();
}

Result<void> PdfDocumentReader::open(const std::filesystem::path& path) {
    std::scoped_lock lock(pdfiumMutex());

    if (document_ != nullptr) {
        ::FPDF_CloseDocument(document_);
        document_ = nullptr;
    }
    streamCtx_.reset();
    pageCount_ = 0;
    metadata_ = {};

    if (path.empty()) {
        return Result<void>::err(ErrorCode::InvalidArgument, "PDF path is empty");
    }

    const std::wstring wideLongPath = FileManager::toWin32LongPath(path);
    HANDLE hFile = ::CreateFileW(
        wideLongPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        const DWORD lastErr = ::GetLastError();
        const ErrorCode code = (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND)
            ? ErrorCode::NotFound
            : ErrorCode::IoError;
        return Result<void>::err(
            code,
            "Failed to open PDF file handle: " + FileManager::wideToUtf8(path.native()));
    }

    LARGE_INTEGER liSize{};
    if (!::GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0) {
        ::CloseHandle(hFile);
        return Result<void>::err(ErrorCode::CorruptArchive, "PDF file is empty or unreadable");
    }

    auto ctx = std::make_unique<StreamContext>();
    ctx->fileHandle = hFile;
    ctx->fileSize = static_cast<std::uint64_t>(liSize.QuadPart);
    ctx->fileAccess.m_FileLen = static_cast<unsigned long>(
        (std::min)(
            ctx->fileSize,
            static_cast<std::uint64_t>((std::numeric_limits<unsigned long>::max)())));
    ctx->fileAccess.m_GetBlock = &StreamContext::customReadCallback;
    ctx->fileAccess.m_Param = ctx.get();

    ensurePdfiumInitializedLocked();

    FPDF_DOCUMENT doc = ::FPDF_LoadCustomDocument(&ctx->fileAccess, nullptr);
    if (doc == nullptr) {
        const unsigned long fpdfErr = ::FPDF_GetLastError();
        return Result<void>::err(
            mapPdfiumError(fpdfErr),
            "PDFium failed to load PDF document (code " + std::to_string(fpdfErr) + ")");
    }

    const int count = ::FPDF_GetPageCount(doc);
    if (count < 0) {
        ::FPDF_CloseDocument(doc);
        return Result<void>::err(ErrorCode::CorruptArchive, "PDFium returned invalid page count");
    }

    PdfMetadata meta;
    meta.title = readPdfMetaTagLocked(doc, "Title");
    meta.author = readPdfMetaTagLocked(doc, "Author");
    meta.subject = readPdfMetaTagLocked(doc, "Subject");
    meta.keywords = readPdfMetaTagLocked(doc, "Keywords");

    streamCtx_ = std::move(ctx);
    document_ = doc;
    pageCount_ = static_cast<std::size_t>(count);
    metadata_ = std::move(meta);

    return Result<void>::ok();
}

void PdfDocumentReader::close() noexcept {
    std::scoped_lock lock(pdfiumMutex());
    if (document_ != nullptr) {
        ::FPDF_CloseDocument(document_);
        document_ = nullptr;
    }
    streamCtx_.reset();
    pageCount_ = 0;
    metadata_ = {};
}

bool PdfDocumentReader::isOpen() const noexcept {
    std::scoped_lock lock(pdfiumMutex());
    return document_ != nullptr;
}

std::size_t PdfDocumentReader::pageCount() const noexcept {
    std::scoped_lock lock(pdfiumMutex());
    return pageCount_;
}

std::string PdfDocumentReader::pageName(std::size_t index) const {
    std::scoped_lock lock(pdfiumMutex());
    if (document_ == nullptr || index >= pageCount_) {
        return {};
    }
    return "page_" + std::to_string(index + 1) + ".pdf";
}

PdfMetadata PdfDocumentReader::pdfMetadata() const {
    std::scoped_lock lock(pdfiumMutex());
    return metadata_;
}

std::uint64_t PdfDocumentReader::fileSizeBytes() const noexcept {
    std::scoped_lock lock(pdfiumMutex());
    return streamCtx_ != nullptr ? streamCtx_->fileSize : 0;
}

Result<std::vector<std::uint8_t>> PdfDocumentReader::tryExtractRawPageImage(
    std::size_t index,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::Cancelled, "PDF raw image extraction cancelled");
    }

    std::scoped_lock lock(pdfiumMutex());
    if (document_ == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::InvalidArgument, "PDF document is not open");
    }
    if (index >= pageCount_) {
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::InvalidArgument, "PDF page index out of range");
    }

    FPDF_PAGE page = ::FPDF_LoadPage(document_, static_cast<int>(index));
    if (page == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::DecodeFailed, "PDFium failed to load page");
    }

    if (::FPDFPage_CountObjects(page) != 1) {
        ::FPDF_ClosePage(page);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::UnsupportedFormat, "PDF page does not contain a single image object");
    }

    FPDF_PAGEOBJECT obj = ::FPDFPage_GetObject(page, 0);
    if (obj == nullptr || ::FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) {
        ::FPDF_ClosePage(page);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::UnsupportedFormat, "PDF page object is not an image");
    }

    if (::FPDFImageObj_GetImageFilterCount(obj) != 1) {
        ::FPDF_ClosePage(page);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::UnsupportedFormat, "PDF image object does not use a single filter");
    }

    const unsigned long filterLen = ::FPDFImageObj_GetImageFilter(obj, 0, nullptr, 0);
    if (filterLen == 0) {
        ::FPDF_ClosePage(page);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::UnsupportedFormat, "PDF image object filter is empty");
    }

    std::string filterName(filterLen, '\0');
    ::FPDFImageObj_GetImageFilter(obj, 0, filterName.data(), filterLen);
    while (!filterName.empty() && filterName.back() == '\0') {
        filterName.pop_back();
    }
    if (filterName != "DCTDecode") {
        ::FPDF_ClosePage(page);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::UnsupportedFormat, "PDF image object is not DCTDecode JPEG");
    }

    const unsigned long rawLen = ::FPDFImageObj_GetImageDataRaw(obj, nullptr, 0);
    if (rawLen < 4) {
        ::FPDF_ClosePage(page);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::DecodeFailed, "PDF raw DCTDecode image data is empty");
    }

    std::vector<std::uint8_t> rawBytes(rawLen);
    const unsigned long bytesRead = ::FPDFImageObj_GetImageDataRaw(obj, rawBytes.data(), rawLen);
    ::FPDF_ClosePage(page);
    if (bytesRead == 0) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::DecodeFailed, "Failed to read raw DCTDecode image data");
    }
    rawBytes.resize(bytesRead);

    if (rawBytes.size() < 2 || rawBytes[0] != 0xFF || rawBytes[1] != 0xD8) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::DecodeFailed, "Extracted DCTDecode stream does not start with JPEG SOI");
    }

    return Result<std::vector<std::uint8_t>>::ok(std::move(rawBytes));
}

Result<ImageFrame> PdfDocumentReader::tryExtractNativePageBitmap(
    std::size_t index,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "PDF native bitmap extraction cancelled");
    }

    std::scoped_lock lock(pdfiumMutex());
    if (document_ == nullptr) {
        return Result<ImageFrame>::err(ErrorCode::InvalidArgument, "PDF document is not open");
    }
    if (index >= pageCount_) {
        return Result<ImageFrame>::err(ErrorCode::InvalidArgument, "PDF page index out of range");
    }

    FPDF_PAGE page = ::FPDF_LoadPage(document_, static_cast<int>(index));
    if (page == nullptr) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "PDFium failed to load page");
    }

    if (::FPDFPage_CountObjects(page) != 1) {
        ::FPDF_ClosePage(page);
        return Result<ImageFrame>::err(
            ErrorCode::UnsupportedFormat, "PDF page does not contain a single image object");
    }

    FPDF_PAGEOBJECT obj = ::FPDFPage_GetObject(page, 0);
    if (obj == nullptr || ::FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) {
        ::FPDF_ClosePage(page);
        return Result<ImageFrame>::err(
            ErrorCode::UnsupportedFormat, "PDF page object is not an image");
    }

    FPDF_BITMAP bitmap = ::FPDFImageObj_GetBitmap(obj);
    if (bitmap == nullptr) {
        bitmap = ::FPDFImageObj_GetRenderedBitmap(document_, page, obj);
    }
    if (bitmap == nullptr) {
        ::FPDF_ClosePage(page);
        return Result<ImageFrame>::err(
            ErrorCode::DecodeFailed, "PDFium failed to extract native image bitmap");
    }

    const int w = ::FPDFBitmap_GetWidth(bitmap);
    const int h = ::FPDFBitmap_GetHeight(bitmap);
    const int srcStride = ::FPDFBitmap_GetStride(bitmap);
    const int fmt = ::FPDFBitmap_GetFormat(bitmap);
    const auto* srcBuf = static_cast<const std::uint8_t*>(::FPDFBitmap_GetBuffer(bitmap));

    if (w <= 0 || h <= 0 || srcStride <= 0 || srcBuf == nullptr) {
        ::FPDFBitmap_Destroy(bitmap);
        ::FPDF_ClosePage(page);
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "Invalid PDF native image bitmap");
    }

    ImageFrame frame;
    frame.width = static_cast<std::uint32_t>(w);
    frame.height = static_cast<std::uint32_t>(h);
    frame.stride = frame.width * 4u;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * static_cast<std::size_t>(h));

    bool supported = true;
    for (int y = 0; y < h && supported; ++y) {
        const std::uint8_t* srcRow = srcBuf + static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStride);
        std::uint8_t* dstRow = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride;
        switch (fmt) {
            case FPDFBitmap_Gray:
                for (int x = 0; x < w; ++x) {
                    const std::uint8_t g = srcRow[x];
                    const std::size_t d = static_cast<std::size_t>(x) * 4u;
                    dstRow[d + 0] = g;
                    dstRow[d + 1] = g;
                    dstRow[d + 2] = g;
                    dstRow[d + 3] = 255;
                }
                break;
            case FPDFBitmap_BGR:
                for (int x = 0; x < w; ++x) {
                    const std::size_t s = static_cast<std::size_t>(x) * 3u;
                    const std::size_t d = static_cast<std::size_t>(x) * 4u;
                    dstRow[d + 0] = srcRow[s + 2]; // R
                    dstRow[d + 1] = srcRow[s + 1]; // G
                    dstRow[d + 2] = srcRow[s + 0]; // B
                    dstRow[d + 3] = 255;
                }
                break;
            case FPDFBitmap_BGRx:
                for (int x = 0; x < w; ++x) {
                    const std::size_t idx = static_cast<std::size_t>(x) * 4u;
                    dstRow[idx + 0] = srcRow[idx + 2]; // R
                    dstRow[idx + 1] = srcRow[idx + 1]; // G
                    dstRow[idx + 2] = srcRow[idx + 0]; // B
                    dstRow[idx + 3] = 255;
                }
                break;
            case FPDFBitmap_BGRA:
                for (int x = 0; x < w; ++x) {
                    const std::size_t idx = static_cast<std::size_t>(x) * 4u;
                    dstRow[idx + 0] = srcRow[idx + 2]; // R
                    dstRow[idx + 1] = srcRow[idx + 1]; // G
                    dstRow[idx + 2] = srcRow[idx + 0]; // B
                    dstRow[idx + 3] = srcRow[idx + 3]; // A
                }
                break;
            default:
                supported = false;
                break;
        }
    }

    ::FPDFBitmap_Destroy(bitmap);
    ::FPDF_ClosePage(page);

    if (!supported) {
        return Result<ImageFrame>::err(ErrorCode::UnsupportedFormat, "Unsupported PDF native bitmap format");
    }
    return Result<ImageFrame>::ok(std::move(frame));
}

Result<ImageFrame> PdfDocumentReader::renderPage(
    std::size_t index,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "PDF page render cancelled");
    }

    std::scoped_lock lock(pdfiumMutex());
    if (document_ == nullptr) {
        return Result<ImageFrame>::err(ErrorCode::InvalidArgument, "PDF document is not open");
    }
    if (index >= pageCount_) {
        return Result<ImageFrame>::err(ErrorCode::InvalidArgument, "PDF page index out of range");
    }
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "PDF page render cancelled");
    }

    FPDF_PAGE page = ::FPDF_LoadPage(document_, static_cast<int>(index));
    if (page == nullptr) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "PDFium failed to load page");
    }

    const double pageWidthPt = ::FPDF_GetPageWidth(page);
    const double pageHeightPt = ::FPDF_GetPageHeight(page);
    if (pageWidthPt <= 0.0 || pageHeightPt <= 0.0) {
        ::FPDF_ClosePage(page);
        return Result<ImageFrame>::err(ErrorCode::CorruptArchive, "Invalid PDF page dimensions");
    }

    std::uint32_t outW = 0;
    std::uint32_t outH = 0;
    if (targetWidth > 0 && targetHeight > 0) {
        outW = targetWidth;
        outH = targetHeight;
    } else if (targetWidth > 0) {
        outW = targetWidth;
        outH = static_cast<std::uint32_t>(
            (std::max)(1L, std::lround(static_cast<double>(targetWidth) * (pageHeightPt / pageWidthPt))));
    } else if (targetHeight > 0) {
        outH = targetHeight;
        outW = static_cast<std::uint32_t>(
            (std::max)(1L, std::lround(static_cast<double>(targetHeight) * (pageWidthPt / pageHeightPt))));
    } else {
        outW = static_cast<std::uint32_t>((std::max)(1L, std::lround(pageWidthPt * kDefaultRenderScale)));
        outH = static_cast<std::uint32_t>((std::max)(1L, std::lround(pageHeightPt * kDefaultRenderScale)));
    }

    outW = (std::min)(outW, kMaxRenderDimension);
    outH = (std::min)(outH, kMaxRenderDimension);

    if (stopToken.stop_requested()) {
        ::FPDF_ClosePage(page);
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "PDF page render cancelled");
    }

    const int stride = static_cast<int>(outW * 4u);
    std::vector<std::uint8_t> bgraBuffer(static_cast<std::size_t>(stride) * outH, 0xFF);

    FPDF_BITMAP bitmap = ::FPDFBitmap_CreateEx(
        static_cast<int>(outW),
        static_cast<int>(outH),
        FPDFBitmap_BGRA,
        bgraBuffer.data(),
        stride);
    if (bitmap == nullptr) {
        ::FPDF_ClosePage(page);
        return Result<ImageFrame>::err(ErrorCode::OutOfMemory, "Failed to allocate PDFium bitmap");
    }

    ::FPDFBitmap_FillRect(bitmap, 0, 0, static_cast<int>(outW), static_cast<int>(outH), 0xFFFFFFFF);
    ::FPDF_RenderPageBitmap(
        bitmap,
        page,
        0,
        0,
        static_cast<int>(outW),
        static_cast<int>(outH),
        0,
        FPDF_ANNOT | FPDF_LCD_TEXT);

    ::FPDFBitmap_Destroy(bitmap);
    ::FPDF_ClosePage(page);

    // Convert BGRA -> RGBA8888 in-place for ImageFrame.
    const std::size_t pixelCount = static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        std::swap(bgraBuffer[i * 4 + 0], bgraBuffer[i * 4 + 2]);
    }

    ImageFrame frame;
    frame.width = outW;
    frame.height = outH;
    frame.stride = static_cast<std::uint32_t>(stride);
    frame.pixels = std::move(bgraBuffer);

    return Result<ImageFrame>::ok(std::move(frame));
}

Result<std::vector<std::uint8_t>> PdfDocumentReader::readPageBytes(
    std::size_t index,
    std::stop_token stopToken) {
    auto frameRes = renderPage(index, 0, 0, stopToken);
    if (!frameRes.isOk()) {
        return Result<std::vector<std::uint8_t>>::err(frameRes.error());
    }
    return Result<std::vector<std::uint8_t>>::ok(encodeRgbaToBmp(frameRes.value()));
}

Result<std::optional<std::string>> PdfDocumentReader::readMetadataXml() {
    std::scoped_lock lock(pdfiumMutex());
    if (document_ == nullptr) {
        return Result<std::optional<std::string>>::err(
            ErrorCode::InvalidArgument, "PDF document is not open");
    }
    return Result<std::optional<std::string>>::ok(std::nullopt);
}

} // namespace mavira::core::archive
