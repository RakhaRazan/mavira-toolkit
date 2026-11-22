#pragma once

#include "core/CommonTypes.hpp"
#include "core/archive/IArchiveReader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

struct fpdf_document_t__;
typedef struct fpdf_document_t__* FPDF_DOCUMENT;

namespace mavira::core::archive {

struct PdfMetadata {
    std::string title{};
    std::string author{};
    std::string subject{};
    std::string keywords{};
};

/// Thread-safe, 64-bit Win32 streaming PDF reader powered by PDFium.
/// Uses `CreateFileW` + `SetFilePointerEx` (`LARGE_INTEGER`) + `ReadFile` via
/// `FPDF_FILEACCESS` (`FPDF_LoadCustomDocument`) so multi-GB PDFs stream only
/// requested page objects into RAM. All `FPDF_*` calls are guarded by a dedicated mutex.
class PdfDocumentReader final : public IArchiveReader {
public:
    PdfDocumentReader();
    ~PdfDocumentReader() override;

    PdfDocumentReader(const PdfDocumentReader&) = delete;
    PdfDocumentReader& operator=(const PdfDocumentReader&) = delete;
    PdfDocumentReader(PdfDocumentReader&&) = delete;
    PdfDocumentReader& operator=(PdfDocumentReader&&) = delete;

    [[nodiscard]] mavira::core::Result<void> open(const std::filesystem::path& path) override;
    void close() noexcept override;
    [[nodiscard]] bool isOpen() const noexcept override;

    [[nodiscard]] std::size_t pageCount() const noexcept override;
    [[nodiscard]] std::string pageName(std::size_t index) const override;

    /// Renders page `index` into an in-memory 32-bit BMP byte buffer.
    [[nodiscard]] mavira::core::Result<std::vector<std::uint8_t>> readPageBytes(
        std::size_t index,
        std::stop_token stopToken = {}) override;

    /// PDFs store metadata in the Info dictionary rather than `ComicInfo.xml`;
    /// returns `ok(std::nullopt)` and exposes extracted fields via `pdfMetadata()`.
    [[nodiscard]] mavira::core::Result<std::optional<std::string>> readMetadataXml() override;

    /// Attempts to extract a raw embedded JPEG (`DCTDecode`) image directly from page `index`
    /// when the page contains a single image object, bypassing full bitmap rasterization.
    [[nodiscard]] mavira::core::Result<std::vector<std::uint8_t>> tryExtractRawPageImage(
        std::size_t index,
        std::stop_token stopToken = {});

    /// Attempts to extract the native embedded image bitmap directly from page `index`
    /// when the page contains a single image object (e.g. FlateDecode, JPXDecode, CCITTFaxDecode),
    /// bypassing slow 300-DPI vector page rasterization.
    [[nodiscard]] mavira::core::Result<mavira::core::ImageFrame> tryExtractNativePageBitmap(
        std::size_t index,
        std::stop_token stopToken = {});

    /// Renders page `index` directly into an RGBA8888 `ImageFrame`.
    /// When `targetWidth` and `targetHeight` are 0, renders at ~150 DPI native scale.
    [[nodiscard]] mavira::core::Result<mavira::core::ImageFrame> renderPage(
        std::size_t index,
        std::uint32_t targetWidth = 0,
        std::uint32_t targetHeight = 0,
        std::stop_token stopToken = {});

    /// Returns the extracted PDF document Info dictionary metadata.
    [[nodiscard]] PdfMetadata pdfMetadata() const;

    /// Returns the 64-bit byte size of the currently open PDF file.
    [[nodiscard]] std::uint64_t fileSizeBytes() const noexcept;

    /// Returns the dedicated global mutex guarding all PDFium `FPDF_*` calls.
    [[nodiscard]] static std::mutex& pdfiumMutex() noexcept;

private:
    struct StreamContext;

    std::unique_ptr<StreamContext> streamCtx_{};
    FPDF_DOCUMENT document_{nullptr};
    std::size_t pageCount_{0};
    PdfMetadata metadata_{};
};

} // namespace mavira::core::archive
