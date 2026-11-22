#include "core/rendering/RenderingEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mavira::core::rendering {

namespace {

using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;

[[nodiscard]] inline bool isWhitePixel(
    const std::uint8_t* px,
    std::uint8_t whiteThreshold) noexcept {
    // Fully transparent pixels are treated as blank border.
    if (px[3] == 0) {
        return true;
    }
    return px[0] >= whiteThreshold && px[1] >= whiteThreshold && px[2] >= whiteThreshold;
}

[[nodiscard]] inline bool isBlackPixel(
    const std::uint8_t* px,
    std::uint8_t blackThreshold) noexcept {
    if (px[3] == 0) {
        return true;
    }
    return px[0] <= blackThreshold && px[1] <= blackThreshold && px[2] <= blackThreshold;
}

[[nodiscard]] bool isUniformBorderRow(
    const ImageFrame& frame,
    std::uint32_t y,
    const AutoCropOptions& options) noexcept {
    const std::uint32_t stride = frame.effectiveStride();
    const std::uint8_t* row = frame.pixels.data() + static_cast<std::size_t>(y) * stride;
    const std::uint32_t width = frame.width;
    const std::uint32_t maxNoise = static_cast<std::uint32_t>(
        std::floor(static_cast<float>(width) * (std::clamp)(options.borderToleranceRatio, 0.0f, 0.25f)));

    std::uint32_t nonWhite = 0;
    std::uint32_t nonBlack = 0;

    for (std::uint32_t x = 0; x < width; ++x) {
        const std::uint8_t* px = row + static_cast<std::size_t>(x) * ImageFrame::kBytesPerPixel;
        if (!isWhitePixel(px, options.whiteThreshold)) {
            ++nonWhite;
        }
        if (!isBlackPixel(px, options.blackThreshold)) {
            ++nonBlack;
        }
        if (nonWhite > maxNoise && nonBlack > maxNoise) {
            return false;
        }
    }

    return nonWhite <= maxNoise || nonBlack <= maxNoise;
}

[[nodiscard]] bool isUniformBorderColumn(
    const ImageFrame& frame,
    std::uint32_t x,
    std::uint32_t topY,
    std::uint32_t bottomY,
    const AutoCropOptions& options) noexcept {
    const std::uint32_t stride = frame.effectiveStride();
    const std::uint32_t spanHeight = bottomY - topY + 1;
    const std::uint32_t maxNoise = static_cast<std::uint32_t>(
        std::floor(static_cast<float>(spanHeight) * (std::clamp)(options.borderToleranceRatio, 0.0f, 0.25f)));

    std::uint32_t nonWhite = 0;
    std::uint32_t nonBlack = 0;
    const std::size_t xOffset = static_cast<std::size_t>(x) * ImageFrame::kBytesPerPixel;

    for (std::uint32_t y = topY; y <= bottomY; ++y) {
        const std::uint8_t* px = frame.pixels.data() + static_cast<std::size_t>(y) * stride + xOffset;
        if (!isWhitePixel(px, options.whiteThreshold)) {
            ++nonWhite;
        }
        if (!isBlackPixel(px, options.blackThreshold)) {
            ++nonBlack;
        }
        if (nonWhite > maxNoise && nonBlack > maxNoise) {
            return false;
        }
    }

    return nonWhite <= maxNoise || nonBlack <= maxNoise;
}

[[nodiscard]] ImageFrame extractSubFrame(
    const ImageFrame& src,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) {
    ImageFrame dst;
    dst.width = width;
    dst.height = height;
    dst.stride = width * ImageFrame::kBytesPerPixel;
    dst.pixels.resize(static_cast<std::size_t>(dst.stride) * static_cast<std::size_t>(height));

    const std::uint32_t srcStride = src.effectiveStride();
    const std::size_t srcXBytes = static_cast<std::size_t>(x) * ImageFrame::kBytesPerPixel;

    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* srcRow =
            src.pixels.data() + static_cast<std::size_t>(y + row) * srcStride + srcXBytes;
        std::uint8_t* dstRow =
            dst.pixels.data() + static_cast<std::size_t>(row) * dst.stride;
        std::memcpy(dstRow, srcRow, dst.stride);
    }

    return dst;
}

void blitFrame(
    ImageFrame& canvas,
    const ImageFrame& src,
    std::uint32_t dstX,
    std::uint32_t dstY) {
    const std::uint32_t srcStride = src.effectiveStride();
    const std::uint32_t dstStride = canvas.effectiveStride();
    const std::size_t rowBytes = static_cast<std::size_t>(src.width) * ImageFrame::kBytesPerPixel;
    const std::size_t dstXBytes = static_cast<std::size_t>(dstX) * ImageFrame::kBytesPerPixel;

    for (std::uint32_t y = 0; y < src.height; ++y) {
        const std::uint8_t* srcRow = src.pixels.data() + static_cast<std::size_t>(y) * srcStride;
        std::uint8_t* dstRow =
            canvas.pixels.data() + static_cast<std::size_t>(dstY + y) * dstStride + dstXBytes;
        std::memcpy(dstRow, srcRow, rowBytes);
    }
}

} // namespace

bool RenderingEngine::isWideSpread(std::uint32_t width, std::uint32_t height) noexcept {
    return width > 0 && height > 0 && width > height;
}

bool RenderingEngine::isWideSpread(const ImageFrame& frame) noexcept {
    return frame.isValid() && isWideSpread(frame.width, frame.height);
}

int RenderingEngine::normalizeRotation(int degrees) noexcept {
    int normalized = degrees % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    // Snap to nearest right angle (0, 90, 180, 270).
    const int quadrant = ((normalized + 45) / 90) % 4;
    return quadrant * 90;
}

CropRect RenderingEngine::detectCropRect(
    const ImageFrame& frame,
    const AutoCropOptions& options) {
    if (!frame.isValid()) {
        return CropRect{};
    }

    const CropRect fullRect{0, 0, frame.width, frame.height};
    const std::uint32_t minW = (std::min)(options.minRemainingWidth, frame.width);
    const std::uint32_t minH = (std::min)(options.minRemainingHeight, frame.height);

    std::uint32_t top = 0;
    while (top < frame.height && isUniformBorderRow(frame, top, options)) {
        ++top;
    }
    if (top >= frame.height) {
        // Entire image is a uniform color (e.g., intentional blank or black page).
        return fullRect;
    }

    std::uint32_t bottom = frame.height - 1;
    while (bottom > top && isUniformBorderRow(frame, bottom, options)) {
        --bottom;
    }

    std::uint32_t left = 0;
    while (left < frame.width && isUniformBorderColumn(frame, left, top, bottom, options)) {
        ++left;
    }
    if (left >= frame.width) {
        return fullRect;
    }

    std::uint32_t right = frame.width - 1;
    while (right > left && isUniformBorderColumn(frame, right, top, bottom, options)) {
        --right;
    }

    const std::uint32_t croppedW = right - left + 1;
    const std::uint32_t croppedH = bottom - top + 1;
    if (croppedW < minW || croppedH < minH) {
        return fullRect;
    }

    return CropRect{left, top, croppedW, croppedH};
}

ImageFrame RenderingEngine::autoCropBorders(
    const ImageFrame& frame,
    const AutoCropOptions& options) {
    if (!frame.isValid()) {
        return {};
    }

    const CropRect rect = detectCropRect(frame, options);
    if (rect.x == 0 && rect.y == 0 && rect.width == frame.width && rect.height == frame.height) {
        return frame;
    }

    return extractSubFrame(frame, rect.x, rect.y, rect.width, rect.height);
}

Result<ImageFrame> RenderingEngine::stitchDualPages(
    const ImageFrame& primaryPage,
    const ImageFrame& secondaryPage,
    ReadingDirection direction,
    std::uint32_t gapPx) {
    if (!primaryPage.isValid() || !secondaryPage.isValid()) {
        return Result<ImageFrame>::err(
            ErrorCode::InvalidArgument,
            "Both primary and secondary pages must be valid for dual-page stitching");
    }

    // LTR Comic: [primary (N) | secondary (N+1)]
    // RTL Manga: [secondary (N+1) | primary (N)]
    const ImageFrame& leftPage = (direction == ReadingDirection::RTL) ? secondaryPage : primaryPage;
    const ImageFrame& rightPage = (direction == ReadingDirection::RTL) ? primaryPage : secondaryPage;

    const std::uint64_t totalWidth64 =
        static_cast<std::uint64_t>(leftPage.width) +
        static_cast<std::uint64_t>(gapPx) +
        static_cast<std::uint64_t>(rightPage.width);
    const std::uint32_t totalHeight = (std::max)(leftPage.height, rightPage.height);

    if (totalWidth64 == 0 || totalHeight == 0 || totalWidth64 > 0x3FFFFFFFULL) {
        return Result<ImageFrame>::err(
            ErrorCode::InvalidArgument,
            "Stitched dual-page dimensions are invalid or exceed limits");
    }

    ImageFrame stitched;
    stitched.width = static_cast<std::uint32_t>(totalWidth64);
    stitched.height = totalHeight;
    stitched.stride = stitched.width * ImageFrame::kBytesPerPixel;
    stitched.pixels.resize(
        static_cast<std::size_t>(stitched.stride) * static_cast<std::size_t>(stitched.height));

    // Fill canvas with opaque black (R=0, G=0, B=0, A=255) for clean gutters and vertical padding.
    for (std::size_t i = 0; i < stitched.pixels.size(); i += ImageFrame::kBytesPerPixel) {
        stitched.pixels[i + 0] = 0;
        stitched.pixels[i + 1] = 0;
        stitched.pixels[i + 2] = 0;
        stitched.pixels[i + 3] = 255;
    }

    const std::uint32_t leftOffsetY = (totalHeight - leftPage.height) / 2;
    const std::uint32_t rightOffsetY = (totalHeight - rightPage.height) / 2;
    const std::uint32_t rightOffsetX = leftPage.width + gapPx;

    blitFrame(stitched, leftPage, 0, leftOffsetY);
    blitFrame(stitched, rightPage, rightOffsetX, rightOffsetY);

    return Result<ImageFrame>::ok(std::move(stitched));
}

std::vector<TextureTile> RenderingEngine::splitIntoVerticalTiles(
    const ImageFrame& frame,
    std::uint32_t splitThresholdPx,
    std::uint32_t tileHeightPx) {
    std::vector<TextureTile> tiles;
    if (!frame.isValid()) {
        return tiles;
    }

    const std::uint32_t effectiveThreshold =
        (splitThresholdPx > 0) ? splitThresholdPx : kDefaultWebtoonSplitThresholdPx;
    const std::uint32_t effectiveTileHeight =
        (tileHeightPx > 0) ? (std::min)(tileHeightPx, effectiveThreshold) : kDefaultWebtoonTileHeightPx;

    if (frame.height <= effectiveThreshold) {
        TextureTile single;
        single.tileIndex = 0;
        single.offsetY = 0;
        single.frame = frame;
        tiles.push_back(std::move(single));
        return tiles;
    }

    const std::uint32_t tileCount =
        (frame.height + effectiveTileHeight - 1) / effectiveTileHeight;
    tiles.reserve(tileCount);

    std::uint32_t offsetY = 0;
    std::uint32_t tileIndex = 0;
    while (offsetY < frame.height) {
        const std::uint32_t sliceHeight = (std::min)(effectiveTileHeight, frame.height - offsetY);
        TextureTile tile;
        tile.tileIndex = tileIndex++;
        tile.offsetY = offsetY;
        tile.frame = extractSubFrame(frame, 0, offsetY, frame.width, sliceHeight);
        tiles.push_back(std::move(tile));
        offsetY += sliceHeight;
    }

    return tiles;
}

Result<SpreadLayoutResult> RenderingEngine::composePageLayout(
    const ImageFrame& primaryPage,
    const ImageFrame* secondaryPage,
    const LayoutOptions& options) {
    if (!primaryPage.isValid()) {
        return Result<SpreadLayoutResult>::err(
            ErrorCode::InvalidArgument,
            "Primary page frame is invalid");
    }

    ImageFrame processedPrimary = options.autoCropBorders
        ? autoCropBorders(primaryPage, options.cropOptions)
        : primaryPage;

    SpreadLayoutResult result;

    if (options.readingMode == ReadingMode::DualPage) {
        if (options.smartWideSpread && isWideSpread(processedPrimary)) {
            // Smart Page Layout: wide spread (width > height) renders solo even in Dual-Page mode.
            result.isDualSpread = false;
            result.isWideSolo = true;
            result.pagesConsumed = 1;
            result.composedFrame = std::move(processedPrimary);
        } else if (secondaryPage != nullptr && secondaryPage->isValid()) {
            ImageFrame processedSecondary = options.autoCropBorders
                ? autoCropBorders(*secondaryPage, options.cropOptions)
                : *secondaryPage;

            if (options.smartWideSpread && isWideSpread(processedSecondary)) {
                // Secondary page is itself a wide spread, so keep primary solo now
                // and let secondary render solo on the next page turn.
                result.isDualSpread = false;
                result.isWideSolo = false;
                result.pagesConsumed = 1;
                result.composedFrame = std::move(processedPrimary);
            } else {
                auto stitchedRes = stitchDualPages(
                    processedPrimary,
                    processedSecondary,
                    options.direction,
                    options.dualPageGapPx);
                if (!stitchedRes.isOk()) {
                    return Result<SpreadLayoutResult>::err(stitchedRes.error());
                }
                result.isDualSpread = true;
                result.isWideSolo = false;
                result.pagesConsumed = 2;
                result.composedFrame = std::move(stitchedRes.value());
            }
        } else {
            // Last page of the volume or secondary page not yet available.
            result.isDualSpread = false;
            result.isWideSolo = false;
            result.pagesConsumed = 1;
            result.composedFrame = std::move(processedPrimary);
        }
    } else {
        // SinglePage or VerticalWebtoon mode.
        result.isDualSpread = false;
        result.isWideSolo = isWideSpread(processedPrimary);
        result.pagesConsumed = 1;
        result.composedFrame = std::move(processedPrimary);
    }

    result.tiles = splitIntoVerticalTiles(
        result.composedFrame,
        options.webtoonSplitThresholdPx,
        options.webtoonTileHeightPx);

    return Result<SpreadLayoutResult>::ok(std::move(result));
}

ViewportTransform RenderingEngine::computeViewportTransform(
    std::uint32_t imageWidth,
    std::uint32_t imageHeight,
    std::uint32_t viewportWidth,
    std::uint32_t viewportHeight,
    FitMode fitMode,
    float zoomFactor,
    int rotationDegrees) noexcept {
    ViewportTransform transform;
    transform.rotationDegrees = normalizeRotation(rotationDegrees);

    if (imageWidth == 0 || imageHeight == 0) {
        return transform;
    }

    const bool quarterTurn = (transform.rotationDegrees == 90 || transform.rotationDegrees == 270);
    const float baseW = static_cast<float>(quarterTurn ? imageHeight : imageWidth);
    const float baseH = static_cast<float>(quarterTurn ? imageWidth : imageHeight);
    const float clampedZoom = (std::clamp)(zoomFactor, 0.1f, 8.0f);

    float fitScale = 1.0f;
    if (viewportWidth > 0 && viewportHeight > 0) {
        const float scaleX = static_cast<float>(viewportWidth) / baseW;
        const float scaleY = static_cast<float>(viewportHeight) / baseH;
        switch (fitMode) {
            case FitMode::FitWidth:
                fitScale = scaleX;
                break;
            case FitMode::FitHeight:
                fitScale = scaleY;
                break;
            case FitMode::FitScreen:
                fitScale = (std::min)(scaleX, scaleY);
                break;
            case FitMode::Original:
                fitScale = 1.0f;
                break;
        }
    }

    transform.scale = fitScale * clampedZoom;
    transform.contentWidth = baseW * transform.scale;
    transform.contentHeight = baseH * transform.scale;
    return transform;
}

} // namespace mavira::core::rendering
