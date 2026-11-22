#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mavira::core {

enum class ErrorCode : std::uint32_t {
    Ok = 0,
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    IoError,
    CorruptArchive,
    UnsupportedFormat,
    DecodeFailed,
    OutOfMemory,
    Cancelled,
    DatabaseError,
    CloudPlaceholder,
    OfflineVolume
};

[[nodiscard]] constexpr std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:                return "Ok";
        case ErrorCode::Unknown:           return "Unknown";
        case ErrorCode::InvalidArgument:   return "InvalidArgument";
        case ErrorCode::NotFound:          return "NotFound";
        case ErrorCode::AlreadyExists:     return "AlreadyExists";
        case ErrorCode::PermissionDenied:  return "PermissionDenied";
        case ErrorCode::IoError:           return "IoError";
        case ErrorCode::CorruptArchive:    return "CorruptArchive";
        case ErrorCode::UnsupportedFormat: return "UnsupportedFormat";
        case ErrorCode::DecodeFailed:      return "DecodeFailed";
        case ErrorCode::OutOfMemory:       return "OutOfMemory";
        case ErrorCode::Cancelled:         return "Cancelled";
        case ErrorCode::DatabaseError:     return "DatabaseError";
        case ErrorCode::CloudPlaceholder:  return "CloudPlaceholder";
        case ErrorCode::OfflineVolume:     return "OfflineVolume";
    }
    return "Unknown";
}

struct Error {
    ErrorCode code{ErrorCode::Unknown};
    std::string message{};

    [[nodiscard]] bool isOk() const noexcept {
        return code == ErrorCode::Ok;
    }
};

template <typename T>
class Result {
public:
    static Result ok(T value) {
        return Result(std::move(value));
    }

    static Result err(ErrorCode code, std::string message = {}) {
        return Result(Error{code, std::move(message)});
    }

    static Result err(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] bool isOk() const noexcept {
        return hasValue();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasValue();
    }

    [[nodiscard]] const T& value() const& {
        return std::get<T>(storage_);
    }

    [[nodiscard]] T& value() & {
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] T valueOr(T defaultValue) const& {
        if (hasValue()) {
            return std::get<T>(storage_);
        }
        return defaultValue;
    }

    [[nodiscard]] const Error& error() const& {
        return std::get<Error>(storage_);
    }

    [[nodiscard]] ErrorCode errorCode() const noexcept {
        if (hasValue()) {
            return ErrorCode::Ok;
        }
        return std::get<Error>(storage_).code;
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    static Result ok() {
        return Result(std::nullopt);
    }

    static Result err(ErrorCode code, std::string message = {}) {
        return Result(Error{code, std::move(message)});
    }

    static Result err(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] bool isOk() const noexcept {
        return hasValue();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasValue();
    }

    [[nodiscard]] const Error& error() const& {
        return *error_;
    }

    [[nodiscard]] ErrorCode errorCode() const noexcept {
        return error_.has_value() ? error_->code : ErrorCode::Ok;
    }

private:
    explicit Result(std::optional<Error> error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

/// Decoded RGBA8888 image buffer used across codecs, caches, and GPU uploads.
struct ImageFrame {
    static constexpr std::uint32_t kBytesPerPixel = 4; // RGBA8888

    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0}; // Bytes per row (defaults to width * kBytesPerPixel)
    std::vector<std::uint8_t> pixels{};

    [[nodiscard]] std::uint32_t effectiveStride() const noexcept {
        return stride != 0 ? stride : width * kBytesPerPixel;
    }

    [[nodiscard]] bool isValid() const noexcept {
        if (width == 0 || height == 0) {
            return false;
        }
        const std::size_t required =
            static_cast<std::size_t>(effectiveStride()) * static_cast<std::size_t>(height);
        return pixels.size() >= required;
    }

    [[nodiscard]] std::size_t memoryBytes() const noexcept {
        return pixels.size();
    }
};

enum class ComicFormat : std::uint8_t {
    Unknown = 0,
    CBZ,
    CBR,
    CB7,
    CBT,
    EPUB,
    PDF,
    Folder
};

[[nodiscard]] constexpr std::string_view toString(ComicFormat format) noexcept {
    switch (format) {
        case ComicFormat::Unknown: return "Unknown";
        case ComicFormat::CBZ:     return "CBZ";
        case ComicFormat::CBR:     return "CBR";
        case ComicFormat::CB7:     return "CB7";
        case ComicFormat::CBT:     return "CBT";
        case ComicFormat::EPUB:    return "EPUB";
        case ComicFormat::PDF:     return "PDF";
        case ComicFormat::Folder:  return "Folder";
    }
    return "Unknown";
}

struct ComicEntry {
    std::int64_t id{0};
    std::filesystem::path path{};
    std::string title{};
    std::string series{};
    std::string volumeGuid{};
    ComicFormat format{ComicFormat::Unknown};
    std::uint32_t pageCount{0};
    std::uint32_t currentPage{0};
    std::uint64_t fileSizeBytes{0};
    std::int64_t lastModifiedUnix{0};
    bool isOffline{false};
    bool isCloudPlaceholder{false};
};

enum class RequestPriority : std::uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Immediate = 3
};

struct PageRequest {
    std::uint64_t requestId{0};
    std::filesystem::path archivePath{};
    std::uint32_t pageIndex{0};
    std::uint32_t targetWidth{0};  // 0 = native width
    std::uint32_t targetHeight{0}; // 0 = native height
    RequestPriority priority{RequestPriority::Normal};
};

struct MergeJob {
    std::uint64_t jobId{0};
    std::vector<std::filesystem::path> inputPaths{};
    std::filesystem::path outputPath{};
    ComicFormat outputFormat{ComicFormat::CBZ};
    bool preserveMetadata{true};
    bool overwriteExisting{false};
};

/// Unified execution and persistence options for all 6 Mavira utility tools
/// (`convert_comics`, `convert_images`, `create_comic`, `extract_pages`, `extract_ocr`, `merge_comics`).
struct ToolExecutionOptions {
    // Input
    std::vector<std::wstring> inputPaths{};
    bool recursiveFolders{true};
    std::vector<std::string> allowedExtensions{"cbz", "cbr", "cb7", "cbt", "pdf", "epub"};
    std::string archivePassword{};
    int pdfExtractDpi{300};

    // Output
    std::string outputFormat{"cbz"};          // "cbz", "cb7", "cbt", "zip", "epub", "pdf", "jpg", "png", "webp", "txt"
    std::string outputFolderMode{"same_as_input"}; // "custom" or "same_as_input"
    std::wstring customOutputDir{};
    std::string ifExistsPolicy{"rename"};     // "rename", "overwrite", "skip"
    bool keepSubfolderStructure{false};
    int splitIntoFiles{1};
    std::string compressionMode{"store"};     // "store", "balanced", "max"
    int pdfCreationDpi{0};                    // 0 = use image DPI or 300
    bool generateChapterBookmarks{true};

    // Image Processing
    std::string scaleMode{"percentage"};      // "percentage", "max_width", "max_height"
    int scaleValue{100};
    std::string targetImageFormat{"original"}; // "original", "jpg", "png", "webp"
    bool enableLevels{false};
    int blackLevel{0};
    int whiteLevel{255};
    int brightness{0};
    int sharpness{0};
    bool grayscaleManga{false};
    int cropBorderPx{0};
    bool autoCropUniform{false};
    int addBorderPx{0};
    std::string borderColorHex{"#000000"};
    int workerThreads{4};
    int jpgQuality{90};
    int pngCompression{6};
    int webpQuality{85};
    bool webpLossless{false};

    // Per-Tool Specialized Options
    std::string pageRange{};                  // "" = all pages, or e.g. "1-10, 15" (extract_pages, extract_ocr)
    int pageNumberDigits{3};                  // 0 = keep original internal page name, 3 = 001, 4 = 0001 (extract_pages)
    std::string createFolderMode{"per_subfolder"}; // "per_subfolder" or "single_combined" (create_comic)
    bool stripImageMetadata{false};           // Strip EXIF/XMP/ICC metadata (convert_images)
    std::string ocrLanguage{"auto"};          // "auto", "ja", "en", "id", "ko", "zh-Hans", etc. (extract_ocr)
    double ocrUpscaleFactor{1.0};             // 1.0..2.0 pre-OCR upscale factor (extract_ocr)
    std::string ocrOutputFormat{"txt"};       // "txt", "md", "json" (extract_ocr)
    bool ocrIncludePageHeaders{true};         // Include page headers in OCR output (extract_ocr)
    bool ocrFilterNoiseSymbols{true};         // Strip isolated halftone/border OCR noise symbols
    bool ocrNormalizeAllCaps{true};           // Convert >75% uppercase Latin text to Sentence case
    bool mergeNormalizeWidth{false};          // Normalize page widths across merged volumes (merge_comics)

    // Per-Tool Persistence
    bool rememberLastOptions{true};

    [[nodiscard]] bool operator==(const ToolExecutionOptions&) const = default;
};

} // namespace mavira::core
