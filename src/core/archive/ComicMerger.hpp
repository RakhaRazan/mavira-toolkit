#pragma once

#include "core/CommonTypes.hpp"
#include "core/imaging/ImageDecoder.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mavira::data::db {
class DatabaseManager;
}

namespace mavira::core::archive {

/// Live progress snapshot reported during batch merge, extraction, or CBZ creation.
struct MergeProgress {
    std::size_t currentVolumeIndex{0};
    std::size_t totalVolumes{0};
    std::size_t currentPageInVolume{0};
    std::size_t pagesInCurrentVolume{0};
    std::size_t totalPagesProcessed{0};
    std::size_t totalPagesEstimated{0};
    std::string currentVolumeName{};
};

using MergeProgressCallback = std::function<void(const MergeProgress&)>;

/// Represents a chapter bookmark marking the starting page of a merged source volume.
struct ChapterBookmark {
    std::uint32_t startPageIndex{0};
    std::string title{};
};

/// Configuration for `ComicMerger::merge`.
struct MergeOptions {
    std::vector<std::filesystem::path> inputPaths{};
    std::filesystem::path outputPath{};
    mavira::core::ComicFormat outputFormat{mavira::core::ComicFormat::CBZ};
    bool saveChapterBookmarks{true};
    bool overwriteExisting{true};
    int pdfJpegQuality{92};

    /// Optional SQLite database to persist merged comic record + chapter bookmarks.
    mavira::data::db::DatabaseManager* database{nullptr};
};

/// Result metadata produced by a completed merge operation.
struct MergeResult {
    std::filesystem::path outputPath{};
    mavira::core::ComicFormat outputFormat{mavira::core::ComicFormat::CBZ};
    std::size_t totalPages{0};
    std::size_t totalVolumes{0};
    std::vector<ChapterBookmark> chapterBookmarks{};
    std::int64_t savedComicId{0};
};

/// Result metadata produced by `ComicMerger::runToolJob`.
struct ToolJobResult {
    std::string toolId{};
    std::vector<std::filesystem::path> outputPaths{};
    std::size_t totalItemsProcessed{0};
    std::size_t totalPagesProcessed{0};
    std::size_t skippedCount{0};
    bool usedRawPassThrough{false};
    std::vector<ChapterBookmark> chapterBookmarks{};
};

/// Pure C++20 built-in archive utility suite:
/// - **1-page streaming Batch Comic Merger** (< 40 MB RAM, zero temp folder extraction for CBZ/PDF)
///   supporting `.cbr`, `.cbz`, `.cb7`, `.cbt`, `.pdf`, `.epub`, and folder inputs.
/// - **Output `.cbz` / `.zip`**: Zero-recompression `ZIP_CM_STORE` pass-through via `libzip`,
///   prefixed entry names (`V001_0001.jpg`, ...), and embedded `ComicInfo.xml` + SQLite chapter bookmarks.
/// - **Output `.cb7`**: 7-Zip LZMA2 archive via `libarchive` (`archive_write_set_format_7zip`),
///   100% open-source high-compression alternative to `.cbr`.
/// - **Output `.cbt`**: Uncompressed TAR archive via `libarchive` (`archive_write_set_format_pax_restricted`),
///   100% raw byte pass-through.
/// - **Output `.epub`**: Fixed-Layout Comic EPUB3 package via `libzip` with uncompressed `mimetype`,
///   `META-INF/container.xml`, `OEBPS/content.opf`, `OEBPS/toc.ncx`, per-page XHTML, and `OEBPS/images/`.
/// - **Output `.pdf`**: Multi-page PDF 1.4 binary stream writer with JPEG `/DCTDecode` pass-through,
///   lossless `/FlateDecode` (`/DeviceGray` for grayscale manga, `/DeviceRGB` for color), and `/Outlines` bookmarks.
/// - **Extract Pages** (`extractPagesToFolder`) and **Create CBZ** (`createCbzFromFolder`).
class ComicMerger {
public:
    /// Merges multiple ordered comic/PDF inputs into a single `.cbz`, `.cb7`, `.cbt`, `.epub`, or `.pdf` output
    /// using a 1-page streaming pipeline with cooperative `std::stop_token` cancellation.
    [[nodiscard]] static mavira::core::Result<MergeResult> merge(
        const MergeOptions& options,
        const MergeProgressCallback& progressCallback = {},
        std::stop_token stopToken = {});

    /// Extracts all pages from `archivePath` into `outputFolder` one page at a time.
    [[nodiscard]] static mavira::core::Result<std::size_t> extractPagesToFolder(
        const std::filesystem::path& archivePath,
        const std::filesystem::path& outputFolder,
        const MergeProgressCallback& progressCallback = {},
        std::stop_token stopToken = {});

    /// Packages all supported image files inside `inputFolder` (in natural sort order)
    /// into a zero-recompression (`ZIP_CM_STORE`) `.cbz` archive at `outputCbzPath`.
    [[nodiscard]] static mavira::core::Result<std::size_t> createCbzFromFolder(
        const std::filesystem::path& inputFolder,
        const std::filesystem::path& outputCbzPath,
        const MergeProgressCallback& progressCallback = {},
        std::stop_token stopToken = {});

    /// Converts a single comic/document (`.cbr`, `.cb7`, `.pdf`, `.epub`, `.cbz`, `.cbt`, or folder)
    /// to `.cbz`, `.cb7`, `.cbt`, `.epub`, or `.pdf` using the 1-page streaming pipeline.
    [[nodiscard]] static mavira::core::Result<MergeResult> convertComicBook(
        const std::filesystem::path& inputPath,
        const std::filesystem::path& outputPath,
        mavira::core::ComicFormat targetFormat,
        int pdfJpegQuality = 92,
        bool overwriteExisting = true,
        const MergeProgressCallback& progressCallback = {},
        std::stop_token stopToken = {});

    /// Batch converts standalone image files into `outputDir` as JPEG, PNG, or WebP
    /// using `ImageDecoder`, optionally resizing to fit `maxWidth` (0 = keep original width).
    [[nodiscard]] static mavira::core::Result<std::size_t> batchConvertImages(
        const std::vector<std::filesystem::path>& inputFiles,
        const std::filesystem::path& outputDir,
        mavira::core::imaging::ImageFormat targetFormat,
        std::uint32_t maxWidth = 0,
        int quality = 85,
        const MergeProgressCallback& progressCallback = {},
        std::stop_token stopToken = {});

    /// Persists volume chapter bookmarks for `mergedOutputPath` into SQLite (`DatabaseManager`),
    /// upserting the merged comic record if needed and returning its `comic_id`.
    [[nodiscard]] static mavira::core::Result<std::int64_t> persistChapterBookmarks(
        mavira::data::db::DatabaseManager& db,
        const std::filesystem::path& mergedOutputPath,
        mavira::core::ComicFormat outputFormat,
        std::size_t totalPages,
        const std::vector<ChapterBookmark>& bookmarks);

    /// Unified execution entry point for all 6 utility tools
    /// (`convert_comics`, `convert_images`, `create_comic`, `extract_pages`, `extract_ocr`, `merge_comics`)
    /// using `ToolExecutionOptions`, multi-threaded worker execution, collision-safe naming,
    /// `ifExistsPolicy`, `splitIntoFiles`, and `std::stop_token` cancellation.
    [[nodiscard]] static mavira::core::Result<ToolJobResult> runToolJob(
        std::string_view toolId,
        const mavira::core::ToolExecutionOptions& options,
        const MergeProgressCallback& progressCallback = {},
        std::stop_token stopToken = {});
};

} // namespace mavira::core::archive
