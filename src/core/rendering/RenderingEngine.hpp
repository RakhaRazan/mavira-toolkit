#pragma once

#include "core/CommonTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mavira::core::rendering {

enum class ReadingMode : std::uint8_t {
    SinglePage = 0,
    DualPage,
    VerticalWebtoon
};

enum class ReadingDirection : std::uint8_t {
    LTR = 0, // Western Comic: Page N on Left, Page N+1 on Right
    RTL      // Manga: Page N on Right, Page N+1 on Left
};

enum class FitMode : std::uint8_t {
    FitWidth = 0,
    FitHeight,
    FitScreen,
    Original
};

struct AutoCropOptions {
    /// RGB channels >= whiteThreshold (or alpha == 0) are treated as uniform white margin.
    std::uint8_t whiteThreshold{240};
    /// RGB channels <= blackThreshold (or alpha == 0) are treated as uniform black margin.
    std::uint8_t blackThreshold{15};
    /// Fraction of pixels per row/column allowed to deviate from the uniform border color
    /// (handles minor scan dust or JPEG compression noise).
    float borderToleranceRatio{0.01f};
    /// Minimum width/height preserved after cropping; prevents blank pages from collapsing.
    std::uint32_t minRemainingWidth{32};
    std::uint32_t minRemainingHeight{32};
};

struct CropRect {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};

    [[nodiscard]] bool operator==(const CropRect&) const noexcept = default;
};

struct LayoutOptions {
    ReadingMode readingMode{ReadingMode::SinglePage};
    ReadingDirection direction{ReadingDirection::RTL};
    bool smartWideSpread{true};
    bool autoCropBorders{false};
    AutoCropOptions cropOptions{};
    std::uint32_t dualPageGapPx{0};
    /// Images strictly taller than this threshold are sliced into vertical GPU tiles.
    std::uint32_t webtoonSplitThresholdPx{4096};
    /// Maximum height of each vertical GPU tile so GL_MAX_TEXTURE_SIZE is never exceeded.
    std::uint32_t webtoonTileHeightPx{2048};
};

struct TextureTile {
    std::uint32_t tileIndex{0};
    std::uint32_t offsetY{0};
    mavira::core::ImageFrame frame{};
};

struct SpreadLayoutResult {
    bool isDualSpread{false};
    bool isWideSolo{false};
    std::size_t pagesConsumed{1};
    std::uint32_t webtoonPrevHeightPx{0};
    std::uint32_t webtoonCurrentHeightPx{0};
    std::uint32_t webtoonNextHeightPx{0};
    mavira::core::ImageFrame composedFrame{};
    std::vector<TextureTile> tiles{};

    [[nodiscard]] bool isTiled() const noexcept {
        return tiles.size() > 1;
    }

    [[nodiscard]] bool isValid() const noexcept {
        return composedFrame.isValid() && !tiles.empty();
    }
};

struct ViewportTransform {
    float scale{1.0f};
    float contentWidth{0.0f};
    float contentHeight{0.0f};
    int rotationDegrees{0};
};

/// Pure C++20 visual layout processor for MAVIRA's GPU-accelerated reader.
///
/// Implements:
/// - Smart Page Layout (auto-detecting wide spreads `width > height` to render solo
///   even when Dual-Page Spread mode is active)
/// - RTL Manga vs LTR Comic side-by-side stitching
/// - Auto-Crop Borders (trimming uniform white/black margins)
/// - Webtoon Texture Tile Splitting (slicing ultra-tall images > 4096 px into
///   2048 px vertical GPU tiles so `GL_MAX_TEXTURE_SIZE` is never exceeded)
class RenderingEngine {
public:
    static constexpr std::uint32_t kDefaultWebtoonSplitThresholdPx = 4096;
    static constexpr std::uint32_t kDefaultWebtoonTileHeightPx = 2048;

    /// Returns true if the dimensions represent a landscape/double-width spread (`width > height`).
    [[nodiscard]] static bool isWideSpread(std::uint32_t width, std::uint32_t height) noexcept;

    /// Returns true if `frame` is valid and `frame.width > frame.height`.
    [[nodiscard]] static bool isWideSpread(const mavira::core::ImageFrame& frame) noexcept;

    /// Normalizes any rotation angle to {0, 90, 180, 270}.
    [[nodiscard]] static int normalizeRotation(int degrees) noexcept;

    /// Detects the tight content bounding box by trimming uniform white or black borders.
    /// Returns the full image rect if the image is invalid or would shrink below minRemaining*.
    [[nodiscard]] static CropRect detectCropRect(
        const mavira::core::ImageFrame& frame,
        const AutoCropOptions& options = {});

    /// Returns a tightly cropped copy of `frame` with uniform white/black borders removed.
    [[nodiscard]] static mavira::core::ImageFrame autoCropBorders(
        const mavira::core::ImageFrame& frame,
        const AutoCropOptions& options = {});

    /// Stitches two pages side-by-side according to `direction`:
    /// - LTR: `[primaryPage | secondaryPage]`
    /// - RTL: `[secondaryPage | primaryPage]`
    /// Pages of differing heights are vertically centered onto an opaque black canvas.
    [[nodiscard]] static mavira::core::Result<mavira::core::ImageFrame> stitchDualPages(
        const mavira::core::ImageFrame& primaryPage,
        const mavira::core::ImageFrame& secondaryPage,
        ReadingDirection direction,
        std::uint32_t gapPx = 0);

    /// Slices an ultra-tall image (`height > splitThresholdPx`) into contiguous vertical
    /// GPU texture tiles of height <= `tileHeightPx` (default 2048 px).
    /// Images with `height <= splitThresholdPx` are returned as a single tile.
    [[nodiscard]] static std::vector<TextureTile> splitIntoVerticalTiles(
        const mavira::core::ImageFrame& frame,
        std::uint32_t splitThresholdPx = kDefaultWebtoonSplitThresholdPx,
        std::uint32_t tileHeightPx = kDefaultWebtoonTileHeightPx);

    /// Full layout pipeline combining Auto-Crop, Smart Wide-Spread detection,
    /// RTL/LTR dual-page stitching, and vertical GPU texture tile splitting.
    [[nodiscard]] static mavira::core::Result<SpreadLayoutResult> composePageLayout(
        const mavira::core::ImageFrame& primaryPage,
        const mavira::core::ImageFrame* secondaryPage,
        const LayoutOptions& options);

    /// Composes a seamless vertical Webtoon strip from `[prevPage | currentPage | nextPage]`
    /// stacked with zero gap and sliced into vertical GPU tiles <= `webtoonTileHeightPx`.
    [[nodiscard]] static mavira::core::Result<SpreadLayoutResult> composeWebtoonStrip(
        const mavira::core::ImageFrame* prevPage,
        const mavira::core::ImageFrame& currentPage,
        const mavira::core::ImageFrame* nextPage,
        const LayoutOptions& options);

    /// Computes display scale and rotated content dimensions for a target viewport.
    [[nodiscard]] static ViewportTransform computeViewportTransform(
        std::uint32_t imageWidth,
        std::uint32_t imageHeight,
        std::uint32_t viewportWidth,
        std::uint32_t viewportHeight,
        FitMode fitMode,
        float zoomFactor = 1.0f,
        int rotationDegrees = 0) noexcept;
};

} // namespace mavira::core::rendering
