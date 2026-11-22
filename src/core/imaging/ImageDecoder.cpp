#include "core/imaging/ImageDecoder.hpp"

#include <png.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <webp/encode.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mavira::core::imaging {

namespace {

using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> computeFitDimensions(
    std::uint32_t srcW,
    std::uint32_t srcH,
    std::uint32_t maxW,
    std::uint32_t maxH) noexcept {
    if (srcW == 0 || srcH == 0) {
        return {0, 0};
    }
    if (maxW == 0 && maxH == 0) {
        return {srcW, srcH};
    }

    double scale = 1.0;
    if (maxW > 0 && srcW > maxW) {
        scale = (std::min)(scale, static_cast<double>(maxW) / static_cast<double>(srcW));
    }
    if (maxH > 0 && srcH > maxH) {
        scale = (std::min)(scale, static_cast<double>(maxH) / static_cast<double>(srcH));
    }

    const auto outW = static_cast<std::uint32_t>((std::max)(1L, std::lround(srcW * scale)));
    const auto outH = static_cast<std::uint32_t>((std::max)(1L, std::lround(srcH * scale)));
    return {outW, outH};
}

[[nodiscard]] std::uint16_t readLe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] Result<ImageFrame> decodeJpeg(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "JPEG decode cancelled");
    }

    tjhandle handle = ::tjInitDecompress();
    if (handle == nullptr) {
        return Result<ImageFrame>::err(ErrorCode::OutOfMemory, "Failed to initialize TurboJPEG");
    }

    struct TjGuard {
        tjhandle h;
        ~TjGuard() {
            if (h != nullptr) {
                ::tjDestroy(h);
            }
        }
    } guard{handle};

    int width = 0;
    int height = 0;
    int subsamp = 0;
    int colorspace = 0;
    if (::tjDecompressHeader3(
            handle,
            bytes.data(),
            static_cast<unsigned long>(bytes.size()),
            &width,
            &height,
            &subsamp,
            &colorspace) != 0) {
        return Result<ImageFrame>::err(
            ErrorCode::DecodeFailed,
            std::string("TurboJPEG header error: ") + ::tjGetErrorStr2(handle));
    }

    if (width <= 0 || height <= 0) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "Invalid JPEG dimensions");
    }

    // Native DCT scaled decoding (1/8, 1/4, 1/2, 1/1) for low-RAM thumbnails.
    static constexpr tjscalingfactor kDctScales[] = {
        {1, 8},
        {1, 4},
        {1, 2},
        {1, 1}
    };

    int targetW = width;
    int targetH = height;
    if (maxWidth > 0 || maxHeight > 0) {
        for (const tjscalingfactor& sf : kDctScales) {
            const int sw = TJSCALED(width, sf);
            const int sh = TJSCALED(height, sf);
            const bool fitsW = (maxWidth == 0) || (static_cast<std::uint32_t>(sw) >= maxWidth);
            const bool fitsH = (maxHeight == 0) || (static_cast<std::uint32_t>(sh) >= maxHeight);
            if (fitsW && fitsH) {
                targetW = sw;
                targetH = sh;
                break;
            }
        }
    }

    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "JPEG decode cancelled");
    }

    ImageFrame frame;
    frame.width = static_cast<std::uint32_t>(targetW);
    frame.height = static_cast<std::uint32_t>(targetH);
    frame.stride = frame.width * ImageFrame::kBytesPerPixel;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * frame.height);

    if (::tjDecompress2(
            handle,
            bytes.data(),
            static_cast<unsigned long>(bytes.size()),
            frame.pixels.data(),
            targetW,
            static_cast<int>(frame.stride),
            targetH,
            TJPF_RGBA,
            TJFLAG_FASTDCT) != 0) {
        return Result<ImageFrame>::err(
            ErrorCode::DecodeFailed,
            std::string("TurboJPEG decode error: ") + ::tjGetErrorStr2(handle));
    }

    if ((maxWidth > 0 && frame.width > maxWidth) || (maxHeight > 0 && frame.height > maxHeight)) {
        frame = ImageDecoder::resizeFit(frame, maxWidth, maxHeight);
    }

    return Result<ImageFrame>::ok(std::move(frame));
}

struct PngMemReader {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    std::size_t offset{0};
};

void pngReadFromMemory(png_structp pngPtr, png_bytep outBytes, png_size_t byteCount) {
    auto* reader = static_cast<PngMemReader*>(::png_get_io_ptr(pngPtr));
    if (reader == nullptr || reader->offset + byteCount > reader->size) {
        ::png_error(pngPtr, "Unexpected EOF in PNG memory buffer");
        return;
    }
    std::memcpy(outBytes, reader->data + reader->offset, byteCount);
    reader->offset += byteCount;
}

[[nodiscard]] Result<ImageFrame> decodePng(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "PNG decode cancelled");
    }

    png_structp pngPtr = ::png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (pngPtr == nullptr) {
        return Result<ImageFrame>::err(ErrorCode::OutOfMemory, "Failed to create libpng read struct");
    }
    png_infop infoPtr = ::png_create_info_struct(pngPtr);
    if (infoPtr == nullptr) {
        ::png_destroy_read_struct(&pngPtr, nullptr, nullptr);
        return Result<ImageFrame>::err(ErrorCode::OutOfMemory, "Failed to create libpng info struct");
    }

    struct PngGuard {
        png_structp* png;
        png_infop* info;
        ~PngGuard() {
            ::png_destroy_read_struct(png, info, nullptr);
        }
    } guard{&pngPtr, &infoPtr};

    ImageFrame frame;
    std::vector<png_bytep> rowPointers;

    if (setjmp(png_jmpbuf(pngPtr)) != 0) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "libpng failed to decode PNG image");
    }

    PngMemReader memReader{bytes.data(), bytes.size(), 0};
    ::png_set_read_fn(pngPtr, &memReader, &pngReadFromMemory);
    ::png_read_info(pngPtr, infoPtr);

    const png_uint_32 width = ::png_get_image_width(pngPtr, infoPtr);
    const png_uint_32 height = ::png_get_image_height(pngPtr, infoPtr);
    const png_byte colorType = ::png_get_color_type(pngPtr, infoPtr);
    const png_byte bitDepth = ::png_get_bit_depth(pngPtr, infoPtr);

    if (width == 0 || height == 0) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "Invalid PNG dimensions");
    }

    if (bitDepth == 16) {
        ::png_set_strip_16(pngPtr);
    }
    if (colorType == PNG_COLOR_TYPE_PALETTE) {
        ::png_set_palette_to_rgb(pngPtr);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
        ::png_set_expand_gray_1_2_4_to_8(pngPtr);
    }
    if (::png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS) != 0) {
        ::png_set_tRNS_to_alpha(pngPtr);
    }
    if (colorType == PNG_COLOR_TYPE_RGB ||
        colorType == PNG_COLOR_TYPE_GRAY ||
        colorType == PNG_COLOR_TYPE_PALETTE) {
        ::png_set_filler(pngPtr, 0xFF, PNG_FILLER_AFTER);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
        ::png_set_gray_to_rgb(pngPtr);
    }

    ::png_read_update_info(pngPtr, infoPtr);

    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    frame.stride = frame.width * ImageFrame::kBytesPerPixel;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * frame.height);

    rowPointers.resize(frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        rowPointers[y] = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride;
    }

    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "PNG decode cancelled");
    }

    ::png_read_image(pngPtr, rowPointers.data());
    ::png_read_end(pngPtr, nullptr);

    if ((maxWidth > 0 && frame.width > maxWidth) || (maxHeight > 0 && frame.height > maxHeight)) {
        frame = ImageDecoder::resizeFit(frame, maxWidth, maxHeight);
    }

    return Result<ImageFrame>::ok(std::move(frame));
}

[[nodiscard]] Result<ImageFrame> decodeWebP(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "WebP decode cancelled");
    }

    WebPDecoderConfig config;
    if (::WebPInitDecoderConfig(&config) == 0) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "Failed to init WebP decoder config");
    }

    if (::WebPGetFeatures(bytes.data(), bytes.size(), &config.input) != VP8_STATUS_OK) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "Invalid WebP header");
    }

    const auto [outW, outH] = computeFitDimensions(
        static_cast<std::uint32_t>(config.input.width),
        static_cast<std::uint32_t>(config.input.height),
        maxWidth,
        maxHeight);

    if (outW == 0 || outH == 0) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "Invalid WebP dimensions");
    }

    if (outW != static_cast<std::uint32_t>(config.input.width) ||
        outH != static_cast<std::uint32_t>(config.input.height)) {
        config.options.use_scaling = 1;
        config.options.scaled_width = static_cast<int>(outW);
        config.options.scaled_height = static_cast<int>(outH);
    }

    ImageFrame frame;
    frame.width = outW;
    frame.height = outH;
    frame.stride = outW * ImageFrame::kBytesPerPixel;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * outH);

    config.output.colorspace = MODE_RGBA;
    config.output.u.RGBA.rgba = frame.pixels.data();
    config.output.u.RGBA.stride = static_cast<int>(frame.stride);
    config.output.u.RGBA.size = frame.pixels.size();
    config.output.is_external_memory = 1;

    if (stopToken.stop_requested()) {
        ::WebPFreeDecBuffer(&config.output);
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "WebP decode cancelled");
    }

    const VP8StatusCode status = ::WebPDecode(bytes.data(), bytes.size(), &config);
    ::WebPFreeDecBuffer(&config.output);
    if (status != VP8_STATUS_OK) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "libwebp failed to decode image");
    }

    return Result<ImageFrame>::ok(std::move(frame));
}

[[nodiscard]] Result<ImageFrame> decodeBmp(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "BMP decode cancelled");
    }
    if (bytes.size() < 54) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "BMP buffer too small");
    }

    const std::uint32_t pixelOffset = readLe32(bytes.data() + 10);
    const std::uint32_t dibSize = readLe32(bytes.data() + 14);
    if (dibSize < 40 || pixelOffset >= bytes.size()) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "Unsupported BMP header");
    }

    const auto rawW = static_cast<std::int32_t>(readLe32(bytes.data() + 18));
    const auto rawH = static_cast<std::int32_t>(readLe32(bytes.data() + 22));
    const std::uint16_t bpp = readLe16(bytes.data() + 28);
    const std::uint32_t compression = readLe32(bytes.data() + 30);

    if (rawW <= 0 || rawH == 0 || (bpp != 24 && bpp != 32) || compression != 0) {
        return Result<ImageFrame>::err(ErrorCode::UnsupportedFormat, "Unsupported BMP pixel format");
    }

    const bool topDown = rawH < 0;
    const auto width = static_cast<std::uint32_t>(rawW);
    const auto height = static_cast<std::uint32_t>(topDown ? -rawH : rawH);
    const std::size_t srcBytesPerPixel = bpp / 8;
    const std::size_t srcRowStride = ((static_cast<std::size_t>(width) * srcBytesPerPixel) + 3u) & ~3u;

    if (pixelOffset + srcRowStride * height > bytes.size()) {
        return Result<ImageFrame>::err(ErrorCode::DecodeFailed, "BMP pixel data truncated");
    }

    ImageFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * ImageFrame::kBytesPerPixel;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * height);

    for (std::uint32_t y = 0; y < height; ++y) {
        if ((y & 63u) == 0 && stopToken.stop_requested()) {
            return Result<ImageFrame>::err(ErrorCode::Cancelled, "BMP decode cancelled");
        }
        const std::uint32_t srcY = topDown ? y : (height - 1u - y);
        const std::uint8_t* srcRow = bytes.data() + pixelOffset + srcY * srcRowStride;
        std::uint8_t* dstRow = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            dstRow[x * 4 + 0] = srcRow[x * srcBytesPerPixel + 2]; // R
            dstRow[x * 4 + 1] = srcRow[x * srcBytesPerPixel + 1]; // G
            dstRow[x * 4 + 2] = srcRow[x * srcBytesPerPixel + 0]; // B
            dstRow[x * 4 + 3] = (bpp == 32) ? srcRow[x * 4 + 3] : 0xFF;
        }
    }

    if ((maxWidth > 0 && frame.width > maxWidth) || (maxHeight > 0 && frame.height > maxHeight)) {
        frame = ImageDecoder::resizeFit(frame, maxWidth, maxHeight);
    }

    return Result<ImageFrame>::ok(std::move(frame));
}

} // namespace

ImageFormat ImageDecoder::detectFormat(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return ImageFormat::JPEG;
    }
    if (bytes.size() >= 8 &&
        bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
        bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A && bytes[7] == 0x0A) {
        return ImageFormat::PNG;
    }
    if (bytes.size() >= 12 &&
        std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
        std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
        return ImageFormat::WEBP;
    }
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
        return ImageFormat::BMP;
    }
    if (bytes.size() >= 6 &&
        (std::memcmp(bytes.data(), "GIF87a", 6) == 0 ||
         std::memcmp(bytes.data(), "GIF89a", 6) == 0)) {
        return ImageFormat::GIF;
    }
    return ImageFormat::Unknown;
}

Result<ImageFrame> ImageDecoder::decode(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight,
    std::stop_token stopToken) {
    if (bytes.empty()) {
        return Result<ImageFrame>::err(ErrorCode::InvalidArgument, "Image buffer is empty");
    }
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "Image decode cancelled");
    }

    switch (detectFormat(bytes)) {
        case ImageFormat::JPEG:
            return decodeJpeg(bytes, maxWidth, maxHeight, stopToken);
        case ImageFormat::PNG:
            return decodePng(bytes, maxWidth, maxHeight, stopToken);
        case ImageFormat::WEBP:
            return decodeWebP(bytes, maxWidth, maxHeight, stopToken);
        case ImageFormat::BMP:
            return decodeBmp(bytes, maxWidth, maxHeight, stopToken);
        case ImageFormat::GIF:
            return Result<ImageFrame>::err(
                ErrorCode::UnsupportedFormat, "GIF decoding not yet enabled");
        case ImageFormat::Unknown:
        default:
            return Result<ImageFrame>::err(
                ErrorCode::UnsupportedFormat, "Unrecognized image magic bytes");
    }
}

ImageFrame ImageDecoder::resizeFit(
    const ImageFrame& src,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight) {
    if (!src.isValid()) {
        return {};
    }
    const auto [dstW, dstH] = computeFitDimensions(src.width, src.height, maxWidth, maxHeight);
    if (dstW == src.width && dstH == src.height) {
        return src;
    }

    ImageFrame dst;
    dst.width = dstW;
    dst.height = dstH;
    dst.stride = dstW * ImageFrame::kBytesPerPixel;
    dst.pixels.resize(static_cast<std::size_t>(dst.stride) * dstH);

    const std::uint32_t srcStride = src.effectiveStride();
    const double scaleX = static_cast<double>(src.width) / static_cast<double>(dstW);
    const double scaleY = static_cast<double>(src.height) / static_cast<double>(dstH);

    for (std::uint32_t y = 0; y < dstH; ++y) {
        const double fy = (static_cast<double>(y) + 0.5) * scaleY - 0.5;
        const auto y0 = static_cast<std::uint32_t>(std::clamp(std::floor(fy), 0.0, static_cast<double>(src.height - 1)));
        const auto y1 = (std::min)(y0 + 1u, src.height - 1u);
        const double wy = std::clamp(fy - static_cast<double>(y0), 0.0, 1.0);

        const std::uint8_t* row0 = src.pixels.data() + static_cast<std::size_t>(y0) * srcStride;
        const std::uint8_t* row1 = src.pixels.data() + static_cast<std::size_t>(y1) * srcStride;
        std::uint8_t* outRow = dst.pixels.data() + static_cast<std::size_t>(y) * dst.stride;

        for (std::uint32_t x = 0; x < dstW; ++x) {
            const double fx = (static_cast<double>(x) + 0.5) * scaleX - 0.5;
            const auto x0 = static_cast<std::uint32_t>(std::clamp(std::floor(fx), 0.0, static_cast<double>(src.width - 1)));
            const auto x1 = (std::min)(x0 + 1u, src.width - 1u);
            const double wx = std::clamp(fx - static_cast<double>(x0), 0.0, 1.0);

            const std::uint8_t* p00 = row0 + static_cast<std::size_t>(x0) * 4u;
            const std::uint8_t* p10 = row0 + static_cast<std::size_t>(x1) * 4u;
            const std::uint8_t* p01 = row1 + static_cast<std::size_t>(x0) * 4u;
            const std::uint8_t* p11 = row1 + static_cast<std::size_t>(x1) * 4u;

            for (std::size_t c = 0; c < 4; ++c) {
                const double top = p00[c] * (1.0 - wx) + p10[c] * wx;
                const double bot = p01[c] * (1.0 - wx) + p11[c] * wx;
                const double val = top * (1.0 - wy) + bot * wy;
                outRow[x * 4 + c] = static_cast<std::uint8_t>(std::clamp(std::lround(val), 0L, 255L));
            }
        }
    }

    return dst;
}

Result<std::vector<std::uint8_t>> ImageDecoder::encodeWebP(
    const ImageFrame& frame,
    float quality,
    bool lossless) {
    if (!frame.isValid()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Cannot encode invalid ImageFrame to WebP");
    }

    std::uint8_t* output = nullptr;
    const std::size_t encodedSize = lossless
        ? ::WebPEncodeLosslessRGBA(
              frame.pixels.data(),
              static_cast<int>(frame.width),
              static_cast<int>(frame.height),
              static_cast<int>(frame.effectiveStride()),
              &output)
        : ::WebPEncodeRGBA(
              frame.pixels.data(),
              static_cast<int>(frame.width),
              static_cast<int>(frame.height),
              static_cast<int>(frame.effectiveStride()),
              std::clamp(quality, 0.0f, 100.0f),
              &output);

    if (encodedSize == 0 || output == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::DecodeFailed, "libwebp failed to encode RGBA frame");
    }

    std::vector<std::uint8_t> bytes(output, output + encodedSize);
    ::WebPFree(output);
    return Result<std::vector<std::uint8_t>>::ok(std::move(bytes));
}

Result<std::vector<std::uint8_t>> ImageDecoder::encodeJpeg(const ImageFrame& frame, int quality) {
    if (!frame.isValid()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Cannot encode invalid ImageFrame to JPEG");
    }

    tjhandle handle = ::tjInitCompress();
    if (handle == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::OutOfMemory, "Failed to initialize TurboJPEG compressor");
    }

    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    const int rc = ::tjCompress2(
        handle,
        frame.pixels.data(),
        static_cast<int>(frame.width),
        static_cast<int>(frame.effectiveStride()),
        static_cast<int>(frame.height),
        TJPF_RGBA,
        &jpegBuf,
        &jpegSize,
        TJSAMP_420,
        std::clamp(quality, 1, 100),
        TJFLAG_FASTDCT);

    if (rc != 0 || jpegBuf == nullptr) {
        std::string msg = std::string("TurboJPEG encode error: ") + ::tjGetErrorStr2(handle);
        if (jpegBuf != nullptr) {
            ::tjFree(jpegBuf);
        }
        ::tjDestroy(handle);
        return Result<std::vector<std::uint8_t>>::err(ErrorCode::DecodeFailed, std::move(msg));
    }

    std::vector<std::uint8_t> out(jpegBuf, jpegBuf + jpegSize);
    ::tjFree(jpegBuf);
    ::tjDestroy(handle);
    return Result<std::vector<std::uint8_t>>::ok(std::move(out));
}

namespace {

void pngWriteToMemory(png_structp pngPtr, png_bytep data, png_size_t length) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(::png_get_io_ptr(pngPtr));
    if (out != nullptr && data != nullptr && length > 0) {
        out->insert(out->end(), data, data + length);
    }
}

void pngFlushMemory(png_structp) {}

} // namespace

Result<std::vector<std::uint8_t>> ImageDecoder::encodePng(
    const ImageFrame& frame,
    int compressionLevel) {
    if (!frame.isValid()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Cannot encode invalid ImageFrame to PNG");
    }

    png_structp pngPtr = ::png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (pngPtr == nullptr) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::OutOfMemory, "Failed to create libpng write struct");
    }

    png_infop infoPtr = ::png_create_info_struct(pngPtr);
    if (infoPtr == nullptr) {
        ::png_destroy_write_struct(&pngPtr, nullptr);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::OutOfMemory, "Failed to create libpng info struct");
    }

    struct PngWriteGuard {
        png_structp* png;
        png_infop* info;
        ~PngWriteGuard() {
            ::png_destroy_write_struct(png, info);
        }
    } guard{&pngPtr, &infoPtr};

    std::vector<std::uint8_t> output;
    std::vector<png_bytep> rowPointers;

    if (setjmp(png_jmpbuf(pngPtr)) != 0) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::DecodeFailed, "libpng failed to encode PNG image");
    }

    ::png_set_write_fn(pngPtr, &output, &pngWriteToMemory, &pngFlushMemory);
    ::png_set_compression_level(pngPtr, std::clamp(compressionLevel, 0, 9));
    ::png_set_IHDR(
        pngPtr,
        infoPtr,
        static_cast<png_uint_32>(frame.width),
        static_cast<png_uint_32>(frame.height),
        8,
        PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);
    ::png_write_info(pngPtr, infoPtr);

    rowPointers.resize(frame.height);
    const std::uint32_t stride = frame.effectiveStride();
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        rowPointers[y] = const_cast<png_bytep>(
            frame.pixels.data() + static_cast<std::size_t>(y) * stride);
    }

    ::png_write_image(pngPtr, rowPointers.data());
    ::png_write_end(pngPtr, nullptr);

    return Result<std::vector<std::uint8_t>>::ok(std::move(output));
}

namespace {

[[nodiscard]] double lanczos3Kernel(double x) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kA = 3.0;
    const double ax = std::abs(x);
    if (ax < 1e-8) {
        return 1.0;
    }
    if (ax >= kA) {
        return 0.0;
    }
    const double piX = kPi * ax;
    return (std::sin(piX) / piX) * (std::sin(piX / kA) / (piX / kA));
}

[[nodiscard]] ImageFrame resizeLanczos3(
    const ImageFrame& src,
    std::uint32_t dstW,
    std::uint32_t dstH) {
    if (!src.isValid() || dstW == 0 || dstH == 0) {
        return {};
    }
    if (dstW == src.width && dstH == src.height) {
        return src;
    }

    const std::uint32_t srcW = src.width;
    const std::uint32_t srcH = src.height;
    const std::uint32_t srcStride = src.effectiveStride();

    // Pass 1: Horizontal (srcW x srcH -> dstW x srcH) into float RGBA buffer
    std::vector<float> temp(static_cast<std::size_t>(dstW) * srcH * 4u, 0.0f);
    const double scaleX = static_cast<double>(srcW) / static_cast<double>(dstW);
    const double supportX = (scaleX > 1.0) ? (3.0 * scaleX) : 3.0;
    const double invScaleX = (scaleX > 1.0) ? (1.0 / scaleX) : 1.0;

    for (std::uint32_t x = 0; x < dstW; ++x) {
        const double center = (static_cast<double>(x) + 0.5) * scaleX - 0.5;
        const int left = (std::max)(0, static_cast<int>(std::floor(center - supportX)));
        const int right = (std::min)(static_cast<int>(srcW) - 1, static_cast<int>(std::ceil(center + supportX)));

        std::vector<double> weights(static_cast<std::size_t>(right - left + 1));
        double wSum = 0.0;
        for (int sx = left; sx <= right; ++sx) {
            const double w = lanczos3Kernel((static_cast<double>(sx) - center) * invScaleX);
            weights[static_cast<std::size_t>(sx - left)] = w;
            wSum += w;
        }
        const double norm = (wSum > 1e-8) ? (1.0 / wSum) : 1.0;

        for (std::uint32_t y = 0; y < srcH; ++y) {
            const std::uint8_t* row = src.pixels.data() + static_cast<std::size_t>(y) * srcStride;
            double acc[4]{0.0, 0.0, 0.0, 0.0};
            for (int sx = left; sx <= right; ++sx) {
                const double w = weights[static_cast<std::size_t>(sx - left)] * norm;
                const std::uint8_t* p = row + static_cast<std::size_t>(sx) * 4u;
                acc[0] += p[0] * w;
                acc[1] += p[1] * w;
                acc[2] += p[2] * w;
                acc[3] += p[3] * w;
            }
            float* out = temp.data() + (static_cast<std::size_t>(y) * dstW + x) * 4u;
            for (int c = 0; c < 4; ++c) {
                out[c] = static_cast<float>(acc[c]);
            }
        }
    }

    // Pass 2: Vertical (dstW x srcH -> dstW x dstH)
    ImageFrame dst;
    dst.width = dstW;
    dst.height = dstH;
    dst.stride = dstW * ImageFrame::kBytesPerPixel;
    dst.pixels.resize(static_cast<std::size_t>(dst.stride) * dstH);

    const double scaleY = static_cast<double>(srcH) / static_cast<double>(dstH);
    const double supportY = (scaleY > 1.0) ? (3.0 * scaleY) : 3.0;
    const double invScaleY = (scaleY > 1.0) ? (1.0 / scaleY) : 1.0;

    for (std::uint32_t y = 0; y < dstH; ++y) {
        const double center = (static_cast<double>(y) + 0.5) * scaleY - 0.5;
        const int top = (std::max)(0, static_cast<int>(std::floor(center - supportY)));
        const int bottom = (std::min)(static_cast<int>(srcH) - 1, static_cast<int>(std::ceil(center + supportY)));

        std::vector<double> weights(static_cast<std::size_t>(bottom - top + 1));
        double wSum = 0.0;
        for (int sy = top; sy <= bottom; ++sy) {
            const double w = lanczos3Kernel((static_cast<double>(sy) - center) * invScaleY);
            weights[static_cast<std::size_t>(sy - top)] = w;
            wSum += w;
        }
        const double norm = (wSum > 1e-8) ? (1.0 / wSum) : 1.0;

        std::uint8_t* outRow = dst.pixels.data() + static_cast<std::size_t>(y) * dst.stride;
        for (std::uint32_t x = 0; x < dstW; ++x) {
            double acc[4]{0.0, 0.0, 0.0, 0.0};
            for (int sy = top; sy <= bottom; ++sy) {
                const double w = weights[static_cast<std::size_t>(sy - top)] * norm;
                const float* p = temp.data() + (static_cast<std::size_t>(sy) * dstW + x) * 4u;
                acc[0] += p[0] * w;
                acc[1] += p[1] * w;
                acc[2] += p[2] * w;
                acc[3] += p[3] * w;
            }
            for (int c = 0; c < 4; ++c) {
                outRow[x * 4u + c] = static_cast<std::uint8_t>(std::clamp(std::lround(acc[c]), 0L, 255L));
            }
        }
    }

    return dst;
}

[[nodiscard]] ImageFrame cropFrameRect(
    const ImageFrame& src,
    std::uint32_t left,
    std::uint32_t top,
    std::uint32_t width,
    std::uint32_t height) {
    ImageFrame out;
    out.width = width;
    out.height = height;
    out.stride = width * ImageFrame::kBytesPerPixel;
    out.pixels.resize(static_cast<std::size_t>(out.stride) * height);

    const std::uint32_t srcStride = src.effectiveStride();
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* srcRow =
            src.pixels.data() + static_cast<std::size_t>(top + y) * srcStride + static_cast<std::size_t>(left) * 4u;
        std::uint8_t* dstRow = out.pixels.data() + static_cast<std::size_t>(y) * out.stride;
        std::memcpy(dstRow, srcRow, rowBytes);
    }
    return out;
}

[[nodiscard]] bool pixelsClose(const std::uint8_t* a, const std::uint8_t* b, int tolerance = 12) noexcept {
    return std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0])) <= tolerance &&
           std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1])) <= tolerance &&
           std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2])) <= tolerance;
}

[[nodiscard]] ImageFrame autoCropUniformBorders(const ImageFrame& src) {
    if (!src.isValid() || src.width <= 2 || src.height <= 2) {
        return src;
    }
    const std::uint32_t stride = src.effectiveStride();
    const std::uint8_t* ref = src.pixels.data();

    auto isRowUniform = [&](std::uint32_t y) -> bool {
        const std::uint8_t* row = src.pixels.data() + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < src.width; ++x) {
            if (!pixelsClose(row + static_cast<std::size_t>(x) * 4u, ref)) {
                return false;
            }
        }
        return true;
    };

    auto isColUniform = [&](std::uint32_t x, std::uint32_t yMin, std::uint32_t yMax) -> bool {
        for (std::uint32_t y = yMin; y <= yMax; ++y) {
            const std::uint8_t* p =
                src.pixels.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
            if (!pixelsClose(p, ref)) {
                return false;
            }
        }
        return true;
    };

    std::uint32_t top = 0;
    while (top + 1 < src.height && isRowUniform(top)) {
        ++top;
    }
    std::uint32_t bottom = src.height - 1;
    while (bottom > top && isRowUniform(bottom)) {
        --bottom;
    }
    std::uint32_t left = 0;
    while (left + 1 < src.width && isColUniform(left, top, bottom)) {
        ++left;
    }
    std::uint32_t right = src.width - 1;
    while (right > left && isColUniform(right, top, bottom)) {
        --right;
    }

    const std::uint32_t newW = right - left + 1;
    const std::uint32_t newH = bottom - top + 1;
    if (newW == src.width && newH == src.height) {
        return src;
    }
    return cropFrameRect(src, left, top, newW, newH);
}

void parseHexColor(std::string_view hex, std::uint8_t (&rgba)[4]) noexcept {
    rgba[0] = 0;
    rgba[1] = 0;
    rgba[2] = 0;
    rgba[3] = 255;
    if (!hex.empty() && hex.front() == '#') {
        hex.remove_prefix(1);
    }
    auto parseByte = [](std::string_view s) -> std::uint8_t {
        unsigned int val = 0;
        for (char ch : s) {
            val <<= 4;
            if (ch >= '0' && ch <= '9') val |= static_cast<unsigned int>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') val |= static_cast<unsigned int>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') val |= static_cast<unsigned int>(ch - 'A' + 10);
        }
        return static_cast<std::uint8_t>(val & 0xFFu);
    };
    if (hex.size() == 6 || hex.size() == 8) {
        rgba[0] = parseByte(hex.substr(0, 2));
        rgba[1] = parseByte(hex.substr(2, 2));
        rgba[2] = parseByte(hex.substr(4, 2));
        if (hex.size() == 8) {
            rgba[3] = parseByte(hex.substr(6, 2));
        }
    }
}

void applyUnsharpMask(ImageFrame& frame, int sharpness) {
    if (!frame.isValid() || sharpness <= 0 || frame.width < 3 || frame.height < 3) {
        return;
    }
    const double amount = static_cast<double>(std::clamp(sharpness, 0, 200)) / 50.0;
    const std::uint32_t w = frame.width;
    const std::uint32_t h = frame.height;
    const std::uint32_t stride = frame.effectiveStride();
    std::vector<std::uint8_t> copy = frame.pixels;

    for (std::uint32_t y = 1; y + 1 < h; ++y) {
        const std::uint8_t* prevRow = copy.data() + static_cast<std::size_t>(y - 1) * stride;
        const std::uint8_t* currRow = copy.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* nextRow = copy.data() + static_cast<std::size_t>(y + 1) * stride;
        std::uint8_t* dstRow = frame.pixels.data() + static_cast<std::size_t>(y) * stride;

        for (std::uint32_t x = 1; x + 1 < w; ++x) {
            const std::size_t cIdx = static_cast<std::size_t>(x) * 4u;
            for (std::size_t c = 0; c < 3; ++c) {
                const double blur =
                    (prevRow[cIdx - 4 + c] + 2.0 * prevRow[cIdx + c] + prevRow[cIdx + 4 + c] +
                     2.0 * currRow[cIdx - 4 + c] + 4.0 * currRow[cIdx + c] + 2.0 * currRow[cIdx + 4 + c] +
                     nextRow[cIdx - 4 + c] + 2.0 * nextRow[cIdx + c] + nextRow[cIdx + 4 + c]) /
                    16.0;
                const double orig = static_cast<double>(currRow[cIdx + c]);
                const double sharp = orig + amount * (orig - blur);
                dstRow[cIdx + c] = static_cast<std::uint8_t>(std::clamp(std::lround(sharp), 0L, 255L));
            }
        }
    }
}

[[nodiscard]] std::string toLowerAsciiCopy(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        out.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return out;
}

} // namespace

ImageFrame ImageDecoder::resizeExact(
    const ImageFrame& src,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight) {
    return resizeLanczos3(src, targetWidth, targetHeight);
}

bool ImageDecoder::requiresImageReencode(const mavira::core::ToolExecutionOptions& opts) noexcept {
    const std::string fmt = toLowerAsciiCopy(opts.targetImageFormat);
    const std::string scale = toLowerAsciiCopy(opts.scaleMode);
    const bool originalFmt = (fmt == "original" || fmt.empty());
    const bool noScale = (scale == "percentage" && opts.scaleValue == 100);
    return !(originalFmt &&
             noScale &&
             !opts.enableLevels &&
             opts.brightness == 0 &&
             opts.sharpness == 0 &&
             !opts.grayscaleManga &&
             opts.cropBorderPx == 0 &&
             !opts.autoCropUniform &&
             opts.addBorderPx == 0);
}

Result<ImageFrame> ImageDecoder::processFrame(
    const ImageFrame& src,
    const mavira::core::ToolExecutionOptions& opts,
    std::stop_token stopToken) {
    if (!src.isValid()) {
        return Result<ImageFrame>::err(ErrorCode::InvalidArgument, "Invalid source ImageFrame");
    }
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "Image processing cancelled");
    }

    ImageFrame cur = src;

    // 1. Border cropping
    if (opts.cropBorderPx > 0) {
        const auto crop = static_cast<std::uint32_t>(opts.cropBorderPx);
        const std::uint32_t maxCropX = (cur.width > 1) ? (cur.width - 1) / 2 : 0;
        const std::uint32_t maxCropY = (cur.height > 1) ? (cur.height - 1) / 2 : 0;
        const std::uint32_t cx = (std::min)(crop, maxCropX);
        const std::uint32_t cy = (std::min)(crop, maxCropY);
        if (cx > 0 || cy > 0) {
            cur = cropFrameRect(cur, cx, cy, cur.width - 2 * cx, cur.height - 2 * cy);
        }
    }
    if (opts.autoCropUniform) {
        cur = autoCropUniformBorders(cur);
    }

    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "Image processing cancelled");
    }

    // 2. Scaling (Lanczos-3)
    std::uint32_t dstW = cur.width;
    std::uint32_t dstH = cur.height;
    const std::string scaleMode = toLowerAsciiCopy(opts.scaleMode);
    if (opts.scaleValue > 0) {
        if (scaleMode == "percentage" && opts.scaleValue != 100) {
            const double factor = static_cast<double>(opts.scaleValue) / 100.0;
            dstW = static_cast<std::uint32_t>((std::max)(1L, std::lround(cur.width * factor)));
            dstH = static_cast<std::uint32_t>((std::max)(1L, std::lround(cur.height * factor)));
        } else if (scaleMode == "max_width" && cur.width > static_cast<std::uint32_t>(opts.scaleValue)) {
            const double factor = static_cast<double>(opts.scaleValue) / static_cast<double>(cur.width);
            dstW = static_cast<std::uint32_t>(opts.scaleValue);
            dstH = static_cast<std::uint32_t>((std::max)(1L, std::lround(cur.height * factor)));
        } else if (scaleMode == "max_height" && cur.height > static_cast<std::uint32_t>(opts.scaleValue)) {
            const double factor = static_cast<double>(opts.scaleValue) / static_cast<double>(cur.height);
            dstH = static_cast<std::uint32_t>(opts.scaleValue);
            dstW = static_cast<std::uint32_t>((std::max)(1L, std::lround(cur.width * factor)));
        }
    }
    if (dstW != cur.width || dstH != cur.height) {
        cur = resizeLanczos3(cur, dstW, dstH);
    }

    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "Image processing cancelled");
    }

    // 3. Levels, Brightness, Grayscale
    const bool applyLevels = opts.enableLevels;
    const int blk = std::clamp(opts.blackLevel, 0, 254);
    const int wht = std::clamp(opts.whiteLevel, blk + 1, 255);
    const double levelScale = 255.0 / static_cast<double>(wht - blk);
    const int bright = opts.brightness;

    if (applyLevels || bright != 0 || opts.grayscaleManga) {
        // Ensure tightly packed stride when mutating in place
        const std::uint32_t stride = cur.effectiveStride();
        for (std::uint32_t y = 0; y < cur.height; ++y) {
            std::uint8_t* row = cur.pixels.data() + static_cast<std::size_t>(y) * stride;
            for (std::uint32_t x = 0; x < cur.width; ++x) {
                std::uint8_t* p = row + static_cast<std::size_t>(x) * 4u;
                double r = p[0];
                double g = p[1];
                double b = p[2];

                if (applyLevels) {
                    r = std::clamp((r - blk) * levelScale, 0.0, 255.0);
                    g = std::clamp((g - blk) * levelScale, 0.0, 255.0);
                    b = std::clamp((b - blk) * levelScale, 0.0, 255.0);
                }
                if (bright != 0) {
                    r = std::clamp(r + bright, 0.0, 255.0);
                    g = std::clamp(g + bright, 0.0, 255.0);
                    b = std::clamp(b + bright, 0.0, 255.0);
                }
                if (opts.grayscaleManga) {
                    const double yLum = std::clamp(0.299 * r + 0.587 * g + 0.114 * b, 0.0, 255.0);
                    r = g = b = yLum;
                }

                p[0] = static_cast<std::uint8_t>(std::lround(r));
                p[1] = static_cast<std::uint8_t>(std::lround(g));
                p[2] = static_cast<std::uint8_t>(std::lround(b));
            }
        }
    }

    // 4. Unsharp mask sharpening
    if (opts.sharpness > 0) {
        applyUnsharpMask(cur, opts.sharpness);
    }

    // 5. Border padding
    if (opts.addBorderPx > 0) {
        const auto pad = static_cast<std::uint32_t>(opts.addBorderPx);
        ImageFrame padded;
        padded.width = cur.width + 2u * pad;
        padded.height = cur.height + 2u * pad;
        padded.stride = padded.width * ImageFrame::kBytesPerPixel;
        padded.pixels.resize(static_cast<std::size_t>(padded.stride) * padded.height);

        std::uint8_t rgba[4]{0, 0, 0, 255};
        parseHexColor(opts.borderColorHex, rgba);
        for (std::uint32_t y = 0; y < padded.height; ++y) {
            std::uint8_t* row = padded.pixels.data() + static_cast<std::size_t>(y) * padded.stride;
            for (std::uint32_t x = 0; x < padded.width; ++x) {
                std::memcpy(row + static_cast<std::size_t>(x) * 4u, rgba, 4);
            }
        }

        const std::uint32_t curStride = cur.effectiveStride();
        const std::size_t copyBytes = static_cast<std::size_t>(cur.width) * 4u;
        for (std::uint32_t y = 0; y < cur.height; ++y) {
            const std::uint8_t* srcRow = cur.pixels.data() + static_cast<std::size_t>(y) * curStride;
            std::uint8_t* dstRow =
                padded.pixels.data() + static_cast<std::size_t>(y + pad) * padded.stride + static_cast<std::size_t>(pad) * 4u;
            std::memcpy(dstRow, srcRow, copyBytes);
        }
        cur = std::move(padded);
    }

    return Result<ImageFrame>::ok(std::move(cur));
}

Result<std::vector<std::uint8_t>> ImageDecoder::processAndEncode(
    const ImageFrame& src,
    const mavira::core::ToolExecutionOptions& opts,
    ImageFormat sourceFormat,
    std::stop_token stopToken) {
    auto frameRes = processFrame(src, opts, stopToken);
    if (!frameRes) {
        return Result<std::vector<std::uint8_t>>::err(frameRes.error());
    }

    const std::string fmt = toLowerAsciiCopy(opts.targetImageFormat);
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
            return encodePng(frameRes.value(), opts.pngCompression);
        case ImageFormat::WEBP:
            return encodeWebP(
                frameRes.value(),
                static_cast<float>(opts.webpQuality),
                opts.webpLossless);
        case ImageFormat::JPEG:
        default:
            return encodeJpeg(frameRes.value(), opts.jpgQuality);
    }
}

} // namespace mavira::core::imaging
