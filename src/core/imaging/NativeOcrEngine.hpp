#pragma once

#include "core/CommonTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mavira::core::imaging {

/// Axis-aligned bounding rectangle in image pixel coordinates.
struct OcrBoundingRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

/// Single recognized word and its bounding box.
struct OcrWord {
    std::string text{};
    OcrBoundingRect bounds{};
};

/// Single recognized line of text containing ordered words.
struct OcrLine {
    std::string text{};
    std::vector<OcrWord> words{};
};

/// Describes an installed Windows OCR recognizer language pack.
struct OcrLanguageInfo {
    std::string languageTag{};
    std::string displayName{};
};

/// Complete OCR recognition result for a comic page or standalone image.
struct OcrResult {
    std::string text{};
    std::vector<OcrLine> lines{};
    std::string languageTag{};
    std::optional<double> textAngle{};
};

/// Zero-bloat offline Windows 10/11 Native OCR engine wrapping WinRT
/// `Windows::Media::Ocr::OcrEngine` (links `windowsapp.lib`, 0 MB extra installer size).
class NativeOcrEngine {
public:
    /// Returns all OCR recognizer languages currently installed on this Windows machine.
    [[nodiscard]] static std::vector<OcrLanguageInfo> availableLanguages();

    /// Returns true if the given BCP-47 `languageTag` (e.g. "en-US", "ja") is supported.
    [[nodiscard]] static bool isLanguageSupported(std::string_view languageTag);

    /// Returns the maximum image dimension (width or height in pixels) supported by Windows OCR.
    [[nodiscard]] static std::uint32_t maxImageDimension() noexcept;

    /// Post-processes raw OCR text by optionally stripping isolated non-linguistic
    /// noise tokens and converting >75% uppercase Latin text to natural Sentence case,
    /// leaving Japanese, Chinese, and Korean characters completely untouched.
    [[nodiscard]] static std::string cleanOcrText(
        const std::string& rawText,
        bool filterSymbols = true,
        bool normalizeCaps = true);

    /// Runs offline OCR on a decoded RGBA8888 `ImageFrame`.
    /// When `languageTag` is empty or `"auto"`, uses `OcrEngine::TryCreateFromUserProfileLanguages()`.
    [[nodiscard]] static mavira::core::Result<OcrResult> recognizeFrame(
        const mavira::core::ImageFrame& frame,
        std::string_view languageTag = {},
        std::stop_token stopToken = {});

    /// Runs offline OCR on a decoded RGBA8888 `ImageFrame` with optional pre-OCR upscaling
    /// (`upscaleFactor` clamped to `[1.0, 2.0]`).
    [[nodiscard]] static mavira::core::Result<OcrResult> recognizeFrame(
        const mavira::core::ImageFrame& frame,
        std::string_view languageTag,
        double upscaleFactor,
        std::stop_token stopToken = {});

    /// Runs offline OCR on a decoded RGBA8888 `ImageFrame` with pre-OCR upscaling
    /// and configurable post-processing (`filterNoiseSymbols`, `normalizeAllCaps`).
    [[nodiscard]] static mavira::core::Result<OcrResult> recognizeFrame(
        const mavira::core::ImageFrame& frame,
        std::string_view languageTag,
        double upscaleFactor,
        bool filterNoiseSymbols,
        bool normalizeAllCaps,
        std::stop_token stopToken = {});

    /// Decodes encoded image bytes (JPEG/PNG/WEBP/BMP/GIF) via `ImageDecoder` and runs OCR.
    [[nodiscard]] static mavira::core::Result<OcrResult> recognizeImageBytes(
        std::span<const std::uint8_t> bytes,
        std::string_view languageTag = {},
        std::stop_token stopToken = {});

    /// Reads an image file from disk (supporting Win32 long paths) and runs OCR.
    [[nodiscard]] static mavira::core::Result<OcrResult> recognizeImageFile(
        const std::filesystem::path& imagePath,
        std::string_view languageTag = {},
        std::stop_token stopToken = {});
};

} // namespace mavira::core::imaging
