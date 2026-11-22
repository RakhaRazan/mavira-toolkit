#include "core/imaging/ImageDecoder.hpp"

#include <gtest/gtest.h>
#include <png.h>

#include <cstdint>
#include <stop_token>
#include <vector>

namespace {

using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::imaging::ImageDecoder;
using mavira::core::imaging::ImageFormat;

ImageFrame makeGradientFrame(std::uint32_t width, std::uint32_t height) {
    ImageFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4u;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * height);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4u;
            frame.pixels[idx + 0] = static_cast<std::uint8_t>((x * 255u) / (width > 1 ? width - 1 : 1));
            frame.pixels[idx + 1] = static_cast<std::uint8_t>((y * 255u) / (height > 1 ? height - 1 : 1));
            frame.pixels[idx + 2] = 128;
            frame.pixels[idx + 3] = 255;
        }
    }
    return frame;
}

std::vector<std::uint8_t> encodePngInMemory(const ImageFrame& frame) {
    std::vector<std::uint8_t> out;
    png_structp pngPtr = ::png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    EXPECT_NE(pngPtr, nullptr);
    png_infop infoPtr = ::png_create_info_struct(pngPtr);
    EXPECT_NE(infoPtr, nullptr);

    ::png_set_write_fn(
        pngPtr,
        &out,
        [](png_structp p, png_bytep data, png_size_t len) {
            auto* vec = static_cast<std::vector<std::uint8_t>*>(::png_get_io_ptr(p));
            vec->insert(vec->end(), data, data + len);
        },
        nullptr);

    ::png_set_IHDR(
        pngPtr,
        infoPtr,
        frame.width,
        frame.height,
        8,
        PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);

    std::vector<png_bytep> rows(frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        rows[y] = const_cast<png_bytep>(frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride);
    }

    ::png_write_info(pngPtr, infoPtr);
    ::png_write_image(pngPtr, rows.data());
    ::png_write_end(pngPtr, nullptr);
    ::png_destroy_write_struct(&pngPtr, &infoPtr);
    return out;
}

TEST(ImageDecoderTest, DetectsMagicBytesForAllSupportedFormats) {
    const std::vector<std::uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0};
    const std::vector<std::uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const std::vector<std::uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const std::vector<std::uint8_t> bmp = {'B', 'M', 0x36, 0x00};
    const std::vector<std::uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a'};
    const std::vector<std::uint8_t> unk = {0x00, 0x01, 0x02, 0x03};

    EXPECT_EQ(ImageDecoder::detectFormat(jpeg), ImageFormat::JPEG);
    EXPECT_EQ(ImageDecoder::detectFormat(png), ImageFormat::PNG);
    EXPECT_EQ(ImageDecoder::detectFormat(webp), ImageFormat::WEBP);
    EXPECT_EQ(ImageDecoder::detectFormat(bmp), ImageFormat::BMP);
    EXPECT_EQ(ImageDecoder::detectFormat(gif), ImageFormat::GIF);
    EXPECT_EQ(ImageDecoder::detectFormat(unk), ImageFormat::Unknown);
}

TEST(ImageDecoderTest, JpegNativeDctScaledDecodingAndCancellation) {
    const ImageFrame src = makeGradientFrame(1600, 1200);
    auto jpegRes = ImageDecoder::encodeJpeg(src, 85);
    ASSERT_TRUE(jpegRes.isOk()) << jpegRes.error().message;
    EXPECT_EQ(ImageDecoder::detectFormat(jpegRes.value()), ImageFormat::JPEG);

    // Full 1/1 decode
    auto fullRes = ImageDecoder::decode(jpegRes.value());
    ASSERT_TRUE(fullRes.isOk()) << fullRes.error().message;
    EXPECT_EQ(fullRes.value().width, 1600u);
    EXPECT_EQ(fullRes.value().height, 1200u);
    EXPECT_TRUE(fullRes.value().isValid());

    // Scaled thumbnail decode (uses 1/8 DCT scaling -> 200x150)
    auto thumbRes = ImageDecoder::decode(jpegRes.value(), 200, 200);
    ASSERT_TRUE(thumbRes.isOk()) << thumbRes.error().message;
    EXPECT_LE(thumbRes.value().width, 200u);
    EXPECT_LE(thumbRes.value().height, 200u);
    EXPECT_EQ(thumbRes.value().width, 200u);
    EXPECT_EQ(thumbRes.value().height, 150u);

    // Cooperative cancellation
    std::stop_source stopSource;
    stopSource.request_stop();
    auto cancelRes = ImageDecoder::decode(jpegRes.value(), 0, 0, stopSource.get_token());
    EXPECT_FALSE(cancelRes.isOk());
    EXPECT_EQ(cancelRes.errorCode(), ErrorCode::Cancelled);
}

TEST(ImageDecoderTest, PngAndWebpDecodeAndEncodeRoundTrip) {
    const ImageFrame src = makeGradientFrame(360, 510);

    // PNG decode via libpng
    const std::vector<std::uint8_t> pngBytes = encodePngInMemory(src);
    ASSERT_EQ(ImageDecoder::detectFormat(pngBytes), ImageFormat::PNG);
    auto pngDecoded = ImageDecoder::decode(pngBytes, 180, 255);
    ASSERT_TRUE(pngDecoded.isOk()) << pngDecoded.error().message;
    EXPECT_EQ(pngDecoded.value().width, 180u);
    EXPECT_EQ(pngDecoded.value().height, 255u);

    // WebP encode (quality 75) & decode
    auto webpBytesRes = ImageDecoder::encodeWebP(pngDecoded.value(), 75.0f);
    ASSERT_TRUE(webpBytesRes.isOk()) << webpBytesRes.error().message;
    EXPECT_EQ(ImageDecoder::detectFormat(webpBytesRes.value()), ImageFormat::WEBP);

    auto webpDecoded = ImageDecoder::decode(webpBytesRes.value());
    ASSERT_TRUE(webpDecoded.isOk()) << webpDecoded.error().message;
    EXPECT_EQ(webpDecoded.value().width, 180u);
    EXPECT_EQ(webpDecoded.value().height, 255u);
    EXPECT_TRUE(webpDecoded.value().isValid());
}

TEST(ImageDecoderTest, ProcessAndEncodePipelineAllTransformsAndFormats) {
    // 100x80 frame with 10px uniform black border around a colorful 80x60 center
    ImageFrame src;
    src.width = 100;
    src.height = 80;
    src.stride = 100 * 4u;
    src.pixels.assign(static_cast<std::size_t>(src.stride) * src.height, 0);
    for (std::uint32_t y = 0; y < src.height; ++y) {
        for (std::uint32_t x = 0; x < src.width; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * src.width + x) * 4u;
            const bool isBorder = (x < 10 || x >= 90 || y < 10 || y >= 70);
            src.pixels[off + 0] = isBorder ? 0 : static_cast<std::uint8_t>(40 + (x * 2));
            src.pixels[off + 1] = isBorder ? 0 : static_cast<std::uint8_t>(20 + (y * 2));
            src.pixels[off + 2] = isBorder ? 0 : 180;
            src.pixels[off + 3] = 255;
        }
    }

    mavira::core::ToolExecutionOptions opts;
    opts.autoCropUniform = true;       // -> 80x60
    opts.scaleMode = "max_width";
    opts.scaleValue = 40;              // -> 40x30
    opts.enableLevels = true;
    opts.blackLevel = 10;
    opts.whiteLevel = 230;
    opts.brightness = 5;
    opts.sharpness = 25;
    opts.grayscaleManga = true;
    opts.addBorderPx = 4;              // -> 48x38
    opts.borderColorHex = "#102030";

    // Test PNG encode
    opts.targetImageFormat = "png";
    opts.pngCompression = 6;
    auto pngOut = ImageDecoder::processAndEncode(src, opts);
    ASSERT_TRUE(pngOut.isOk()) << pngOut.error().message;
    EXPECT_EQ(ImageDecoder::detectFormat(pngOut.value()), ImageFormat::PNG);
    auto decPng = ImageDecoder::decode(pngOut.value());
    ASSERT_TRUE(decPng.isOk());
    EXPECT_EQ(decPng.value().width, 48u);
    EXPECT_EQ(decPng.value().height, 38u);
    // Border pixel should match #102030, interior pixel should be grayscale (R == G == B)
    EXPECT_EQ(decPng.value().pixels[0], 0x10);
    EXPECT_EQ(decPng.value().pixels[1], 0x20);
    EXPECT_EQ(decPng.value().pixels[2], 0x30);
    const std::size_t centerOff = (19u * 48u + 24u) * 4u;
    EXPECT_EQ(decPng.value().pixels[centerOff + 0], decPng.value().pixels[centerOff + 1]);
    EXPECT_EQ(decPng.value().pixels[centerOff + 1], decPng.value().pixels[centerOff + 2]);

    // Test WebP lossless encode
    opts.targetImageFormat = "webp";
    opts.webpLossless = true;
    auto webpOut = ImageDecoder::processAndEncode(src, opts);
    ASSERT_TRUE(webpOut.isOk()) << webpOut.error().message;
    EXPECT_EQ(ImageDecoder::detectFormat(webpOut.value()), ImageFormat::WEBP);

    // Test JPG encode with percentage scale
    opts.autoCropUniform = false;
    opts.cropBorderPx = 10;
    opts.scaleMode = "percentage";
    opts.scaleValue = 50;
    opts.addBorderPx = 0;
    opts.targetImageFormat = "jpg";
    opts.jpgQuality = 88;
    auto jpgOut = ImageDecoder::processAndEncode(src, opts);
    ASSERT_TRUE(jpgOut.isOk()) << jpgOut.error().message;
    EXPECT_EQ(ImageDecoder::detectFormat(jpgOut.value()), ImageFormat::JPEG);
    auto decJpg = ImageDecoder::decode(jpgOut.value());
    ASSERT_TRUE(decJpg.isOk());
    EXPECT_EQ(decJpg.value().width, 40u);
    EXPECT_EQ(decJpg.value().height, 30u);
}

} // namespace
