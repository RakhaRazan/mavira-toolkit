#pragma once

#include "core/CommonTypes.hpp"

#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace mavira::core::imaging {

enum class ImageFormat : std::uint8_t {
    Unknown = 0,
    JPEG,
    PNG,
    WEBP,
    BMP,
    GIF
};

/// Pure C++20 SIMD-accelerated image decoder & thumbnail encoder backed by
/// `libjpeg-turbo` (with native 1/1, 1/2, 1/4, 1/8 DCT scaling), `libpng`, and `libwebp`.
class ImageDecoder {
public:
    /// Detects image container format by inspecting magic bytes.
    [[nodiscard]] static ImageFormat detectFormat(
        std::span<const std::uint8_t> bytes) noexcept;

    /// Decodes encoded image bytes into an RGBA8888 `ImageFrame`.
    /// When `maxWidth > 0` or `maxHeight > 0`, applies native JPEG DCT scaling
    /// (1/1, 1/2, 1/4, 1/8) or native WebP scaling for fast low-RAM thumbnail decoding.
    [[nodiscard]] static mavira::core::Result<mavira::core::ImageFrame> decode(
        std::span<const std::uint8_t> bytes,
        std::uint32_t maxWidth = 0,
        std::uint32_t maxHeight = 0,
        std::stop_token stopToken = {});

    /// Resizes an RGBA8888 `ImageFrame` preserving aspect ratio to fit within
    /// `maxWidth x maxHeight` (does not upscale if already within bounds).
    [[nodiscard]] static mavira::core::ImageFrame resizeFit(
        const mavira::core::ImageFrame& src,
        std::uint32_t maxWidth,
        std::uint32_t maxHeight);

    /// Resizes an RGBA8888 `ImageFrame` to exact `targetWidth x targetHeight` using Lanczos-3.
    [[nodiscard]] static mavira::core::ImageFrame resizeExact(
        const mavira::core::ImageFrame& src,
        std::uint32_t targetWidth,
        std::uint32_t targetHeight);

    /// Encodes an RGBA8888 `ImageFrame` as compact WebP (default quality 75, or lossless).
    [[nodiscard]] static mavira::core::Result<std::vector<std::uint8_t>> encodeWebP(
        const mavira::core::ImageFrame& frame,
        float quality = 75.0f,
        bool lossless = false);

    /// Encodes an RGBA8888 `ImageFrame` as JPEG via `libjpeg-turbo`.
    [[nodiscard]] static mavira::core::Result<std::vector<std::uint8_t>> encodeJpeg(
        const mavira::core::ImageFrame& frame,
        int quality = 85);

    /// Encodes an RGBA8888 `ImageFrame` as PNG via `libpng` (compressionLevel 0..9).
    [[nodiscard]] static mavira::core::Result<std::vector<std::uint8_t>> encodePng(
        const mavira::core::ImageFrame& frame,
        int compressionLevel = 6);

    /// Returns true if `opts` requires decoding and re-encoding image pixels
    /// (format change, scaling, levels, brightness, sharpening, grayscale, crop, or border).
    [[nodiscard]] static bool requiresImageReencode(
        const mavira::core::ToolExecutionOptions& opts) noexcept;

    /// Applies ToolExecutionOptions image processing pipeline (crop, Lanczos-3 scaling,
    /// levels, brightness, grayscale, unsharp mask sharpening, border padding) to `src`.
    [[nodiscard]] static mavira::core::Result<mavira::core::ImageFrame> processFrame(
        const mavira::core::ImageFrame& src,
        const mavira::core::ToolExecutionOptions& opts,
        std::stop_token stopToken = {});

    /// Processes `src` according to `opts` and encodes to JPG (`libjpeg-turbo`),
    /// PNG (`libpng`), or WebP (`libwebp` lossy/lossless).
    [[nodiscard]] static mavira::core::Result<std::vector<std::uint8_t>> processAndEncode(
        const mavira::core::ImageFrame& src,
        const mavira::core::ToolExecutionOptions& opts,
        ImageFormat sourceFormat = ImageFormat::JPEG,
        std::stop_token stopToken = {});
};

} // namespace mavira::core::imaging
