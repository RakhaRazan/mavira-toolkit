#include "core/imaging/NativeOcrEngine.hpp"

#include "core/imaging/ImageDecoder.hpp"
#include "data/fs/FileManager.hpp"

#include <Windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace mavira::core::imaging {

namespace {

using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;
using mavira::data::fs::FileManager;

void ensureWinRtApartment() noexcept {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& ex) {
        if (ex.code() != RPC_E_CHANGED_MODE) {
            // Unexpected apartment init error; subsequent WinRT calls will report their own HRESULT.
        }
    } catch (...) {
    }
}

[[nodiscard]] std::string toLowerAscii(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        out.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return out;
}

[[nodiscard]] std::optional<winrt::Windows::Globalization::Language> resolveInstalledLanguage(
    std::string_view languageTag) {
    const std::string lowTarget = toLowerAscii(languageTag);
    const auto avail = winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages();
    for (const auto& lang : avail) {
        const std::string availLow = toLowerAscii(winrt::to_string(lang.LanguageTag()));
        if (availLow == lowTarget) {
            return lang;
        }
    }
    for (const auto& lang : avail) {
        const std::string availLow = toLowerAscii(winrt::to_string(lang.LanguageTag()));
        if (availLow.size() > lowTarget.size() &&
            availLow.compare(0, lowTarget.size(), lowTarget) == 0 &&
            availLow[lowTarget.size()] == '-') {
            return lang;
        }
    }
    return std::nullopt;
}

[[nodiscard]] winrt::Windows::Media::Ocr::OcrEngine createEngineForLanguage(
    std::string_view languageTag,
    std::string& resolvedTag,
    std::string& errorMessage) {
    if (languageTag.empty() || toLowerAscii(languageTag) == "auto") {
        auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            errorMessage =
                "No Windows OCR language pack is available for the current user profile languages";
            return nullptr;
        }
        resolvedTag = winrt::to_string(engine.RecognizerLanguage().LanguageTag());
        return engine;
    }

    const winrt::hstring wideTag = winrt::to_hstring(languageTag);
    if (!winrt::Windows::Globalization::Language::IsWellFormed(wideTag)) {
        errorMessage = "Invalid BCP-47 language tag: " + std::string(languageTag);
        return nullptr;
    }

    winrt::Windows::Globalization::Language lang(wideTag);
    if (!winrt::Windows::Media::Ocr::OcrEngine::IsLanguageSupported(lang)) {
        if (auto matched = resolveInstalledLanguage(languageTag); matched.has_value()) {
            lang = *matched;
        } else {
            errorMessage = "Windows OCR language pack not installed for: " + std::string(languageTag);
            return nullptr;
        }
    }

    auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(lang);
    if (!engine) {
        errorMessage = "Failed to initialize Windows OCR engine for: " + std::string(languageTag);
        return nullptr;
    }
    resolvedTag = winrt::to_string(engine.RecognizerLanguage().LanguageTag());
    return engine;
}

/// Applies lightweight grayscale conversion, isolated 1px halftone screentone
/// despeckling, and contrast stretching into a tightly packed RGBA8 buffer.
[[nodiscard]] std::vector<std::uint8_t> preprocessFrameForOcr(
    const ImageFrame& frame) {
    const std::uint32_t width = frame.width;
    const std::uint32_t height = frame.height;
    const std::uint32_t srcStride = frame.effectiveStride();
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;

    std::vector<std::uint8_t> gray(pixelCount);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = frame.pixels.data() + static_cast<std::size_t>(y) * srcStride;
        std::uint8_t* dst = gray.data() + static_cast<std::size_t>(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4u;
            const std::uint32_t lum =
                (77u * px[0] + 150u * px[1] + 29u * px[2] + 128u) >> 8;
            dst[x] = static_cast<std::uint8_t>(lum > 255u ? 255u : lum);
        }
    }

    // Despeckle isolated 1px halftone screentone dots before contrast thresholding.
    std::vector<std::uint8_t> despeckled = gray;
    if (width >= 3 && height >= 3) {
        for (std::uint32_t y = 1; y + 1 < height; ++y) {
            const std::size_t rowOff = static_cast<std::size_t>(y) * width;
            for (std::uint32_t x = 1; x + 1 < width; ++x) {
                const std::uint8_t c = gray[rowOff + x];
                const std::uint8_t n0 = gray[rowOff - width + x - 1];
                const std::uint8_t n1 = gray[rowOff - width + x];
                const std::uint8_t n2 = gray[rowOff - width + x + 1];
                const std::uint8_t n3 = gray[rowOff + x - 1];
                const std::uint8_t n4 = gray[rowOff + x + 1];
                const std::uint8_t n5 = gray[rowOff + width + x - 1];
                const std::uint8_t n6 = gray[rowOff + width + x];
                const std::uint8_t n7 = gray[rowOff + width + x + 1];

                const std::uint8_t minN = (std::min)({n0, n1, n2, n3, n4, n5, n6, n7});
                const std::uint8_t maxN = (std::max)({n0, n1, n2, n3, n4, n5, n6, n7});

                if (c < 185 && minN >= 190) {
                    const std::uint32_t avg =
                        (static_cast<std::uint32_t>(n0) + n1 + n2 + n3 + n4 + n5 + n6 + n7) >> 3;
                    despeckled[rowOff + x] = static_cast<std::uint8_t>(avg);
                } else if (c > 70 && maxN <= 65) {
                    const std::uint32_t avg =
                        (static_cast<std::uint32_t>(n0) + n1 + n2 + n3 + n4 + n5 + n6 + n7) >> 3;
                    despeckled[rowOff + x] = static_cast<std::uint8_t>(avg);
                }
            }
        }
    }

    // Contrast threshold/stretch: snap light paper/screentone to white and ink to black.
    constexpr std::uint32_t kLow = 60u;
    constexpr std::uint32_t kHigh = 200u;
    constexpr std::uint32_t kSpan = kHigh - kLow;

    std::vector<std::uint8_t> rgba(pixelCount * ImageFrame::kBytesPerPixel);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::uint32_t v = despeckled[i];
        std::uint8_t outV = 0;
        if (v <= kLow) {
            outV = 0u;
        } else if (v >= kHigh) {
            outV = 255u;
        } else {
            outV = static_cast<std::uint8_t>(((v - kLow) * 255u) / kSpan);
        }
        const std::size_t dstOff = i * 4u;
        rgba[dstOff + 0] = outV;
        rgba[dstOff + 1] = outV;
        rgba[dstOff + 2] = outV;
        rgba[dstOff + 3] = 255u;
    }
    return rgba;
}

[[nodiscard]] constexpr bool isAsciiUpper(char ch) noexcept {
    return ch >= 'A' && ch <= 'Z';
}

[[nodiscard]] constexpr bool isAsciiLower(char ch) noexcept {
    return ch >= 'a' && ch <= 'z';
}

[[nodiscard]] constexpr bool isAsciiAlpha(char ch) noexcept {
    return isAsciiUpper(ch) || isAsciiLower(ch);
}

[[nodiscard]] constexpr bool isAsciiDigit(char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

[[nodiscard]] constexpr bool isAsciiAlphaNum(char ch) noexcept {
    return isAsciiAlpha(ch) || isAsciiDigit(ch);
}

[[nodiscard]] constexpr char toUpperAsciiChar(char ch) noexcept {
    return isAsciiLower(ch) ? static_cast<char>(ch - 'a' + 'A') : ch;
}

[[nodiscard]] constexpr char toLowerAsciiChar(char ch) noexcept {
    return isAsciiUpper(ch) ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] constexpr bool isStrayNoiseChar(char ch) noexcept {
    return ch == '|' || ch == '~' || ch == '^' || ch == '_' || ch == '=' || ch == '`';
}

[[nodiscard]] constexpr bool isSlashChar(char ch) noexcept {
    return ch == '/' || ch == '\\';
}

[[nodiscard]] constexpr bool isDashOrDotOrNoise(char ch) noexcept {
    return isStrayNoiseChar(ch) || isSlashChar(ch) || ch == '-' || ch == '.';
}

[[nodiscard]] std::string cleanNoiseToken(std::string_view token) {
    // Strip leading/trailing stray symbols and isolated leading/trailing slashes.
    while (!token.empty() && (isStrayNoiseChar(token.front()) || isSlashChar(token.front()))) {
        token.remove_prefix(1);
    }
    while (!token.empty() && (isStrayNoiseChar(token.back()) || isSlashChar(token.back()))) {
        token.remove_suffix(1);
    }
    if (token.empty()) {
        return {};
    }

    // Check if entire token is purely noise/dashes/dots/slashes.
    bool allNoiseOrDashDot = true;
    for (char ch : token) {
        if (!isDashOrDotOrNoise(ch)) {
            allNoiseOrDashDot = false;
            break;
        }
    }
    if (allNoiseOrDashDot) {
        // Keep only a single standalone '-' (handled for line-level pairing later).
        if (token == "-") {
            return "-";
        }
        return {};
    }

    // Remove stray noise chars inside token and collapse/strip repeated dash/dot artifacts.
    std::string out;
    out.reserve(token.size());
    std::size_t i = 0;
    while (i < token.size()) {
        const char ch = token[i];
        if (isStrayNoiseChar(ch)) {
            ++i;
            continue;
        }
        if (ch == '-') {
            std::size_t runEnd = i;
            while (runEnd < token.size() && token[runEnd] == '-') {
                ++runEnd;
            }
            if (runEnd - i == 1) {
                out.push_back('-');
            }
            // Runs of 2+ dashes ("--", "---", ...) are stripped as border/OCR artifacts.
            i = runEnd;
            continue;
        }
        if (ch == '.') {
            std::size_t runEnd = i;
            while (runEnd < token.size() && token[runEnd] == '.') {
                ++runEnd;
            }
            const std::size_t runLen = runEnd - i;
            if (runLen == 1) {
                out.push_back('.');
            } else if (runLen == 3) {
                // Preserve valid 3-dot ellipsis attached to a word (e.g., "Wait...").
                out += "...";
            }
            // Runs of 2 dots ("..") or 4+ dots ("....") are stripped as halftone artifacts.
            i = runEnd;
            continue;
        }
        out.push_back(ch);
        ++i;
    }

    // Re-check if stripping inner artifacts left only dashes/dots.
    bool stillOnlyDashDot = !out.empty();
    for (char ch : out) {
        if (!isDashOrDotOrNoise(ch)) {
            stillOnlyDashDot = false;
            break;
        }
    }
    if (stillOnlyDashDot && out != "-") {
        return {};
    }
    return out;
}

[[nodiscard]] std::string stripNoiseSymbolsFromText(std::string_view rawText) {
    std::string result;
    result.reserve(rawText.size());

    std::size_t pos = 0;
    bool firstOutLine = true;
    while (pos <= rawText.size()) {
        const std::size_t nl = rawText.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
            ? rawText.substr(pos)
            : rawText.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        std::vector<std::string> tokens;
        std::size_t tp = 0;
        while (tp < line.size()) {
            while (tp < line.size() && (line[tp] == ' ' || line[tp] == '\t')) {
                ++tp;
            }
            if (tp >= line.size()) {
                break;
            }
            const std::size_t start = tp;
            while (tp < line.size() && line[tp] != ' ' && line[tp] != '\t') {
                ++tp;
            }
            std::string cleaned = cleanNoiseToken(line.substr(start, tp - start));
            if (!cleaned.empty()) {
                tokens.push_back(std::move(cleaned));
            }
        }

        // Remove leading/trailing or repeated standalone '-' artifacts on the line.
        while (!tokens.empty() && tokens.front() == "-") {
            tokens.erase(tokens.begin());
        }
        while (!tokens.empty() && tokens.back() == "-") {
            tokens.pop_back();
        }
        for (std::size_t ti = 0; ti + 1 < tokens.size();) {
            if (tokens[ti] == "-" && tokens[ti + 1] == "-") {
                while (ti < tokens.size() && tokens[ti] == "-") {
                    tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(ti));
                }
            } else {
                ++ti;
            }
        }

        if (!tokens.empty()) {
            if (!firstOutLine) {
                result.push_back('\n');
            }
            firstOutLine = false;
            for (std::size_t ti = 0; ti < tokens.size(); ++ti) {
                if (ti > 0) {
                    result.push_back(' ');
                }
                result += tokens[ti];
            }
        }

        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }

    return result;
}

[[nodiscard]] std::size_t utf8SequenceLength(unsigned char leadByte) noexcept {
    if ((leadByte & 0x80u) == 0u) return 1;
    if ((leadByte & 0xE0u) == 0xC0u) return 2;
    if ((leadByte & 0xF0u) == 0xE0u) return 3;
    if ((leadByte & 0xF8u) == 0xF0u) return 4;
    return 1;
}

[[nodiscard]] bool isStandaloneIWord(std::string_view text, std::size_t iPos) noexcept {
    if (iPos >= text.size() || (text[iPos] != 'i' && text[iPos] != 'I')) {
        return false;
    }

    // Check preceding character boundary.
    if (iPos > 0) {
        const char prev = text[iPos - 1];
        if (isAsciiAlphaNum(prev)) {
            return false;
        }
        if (prev == '\'' && iPos >= 2 && isAsciiAlphaNum(text[iPos - 2])) {
            return false;
        }
        if (iPos >= 3 &&
            static_cast<unsigned char>(text[iPos - 3]) == 0xE2 &&
            static_cast<unsigned char>(text[iPos - 2]) == 0x80 &&
            static_cast<unsigned char>(text[iPos - 1]) == 0x99 &&
            iPos >= 4 && isAsciiAlphaNum(text[iPos - 4])) {
            return false;
        }
    }

    const std::size_t next = iPos + 1;
    if (next >= text.size()) {
        return true;
    }

    // Check for contractions: I'm, I'll, I've, I'd (ASCII ' or UTF-8 U+2019).
    std::size_t aposLen = 0;
    if (text[next] == '\'') {
        aposLen = 1;
    } else if (next + 2 < text.size() &&
               static_cast<unsigned char>(text[next]) == 0xE2 &&
               static_cast<unsigned char>(text[next + 1]) == 0x80 &&
               static_cast<unsigned char>(text[next + 2]) == 0x99) {
        aposLen = 3;
    }

    if (aposLen > 0) {
        const std::size_t suffixStart = next + aposLen;
        if (suffixStart >= text.size()) {
            return true;
        }
        const char c0 = toLowerAsciiChar(text[suffixStart]);
        std::size_t suffixLen = 0;
        if (c0 == 'm' || c0 == 'd') {
            suffixLen = 1;
        } else if (suffixStart + 1 < text.size()) {
            const char c1 = toLowerAsciiChar(text[suffixStart + 1]);
            if ((c0 == 'l' && c1 == 'l') || (c0 == 'v' && c1 == 'e')) {
                suffixLen = 2;
            }
        }
        if (suffixLen == 0) {
            return false;
        }
        const std::size_t afterSuffix = suffixStart + suffixLen;
        if (afterSuffix >= text.size()) {
            return true;
        }
        return !isAsciiAlphaNum(text[afterSuffix]);
    }

    return !isAsciiAlphaNum(text[next]);
}

[[nodiscard]] std::string normalizeAllCapsToSentenceCase(std::string_view text) {
    std::size_t upperCount = 0;
    std::size_t lowerCount = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        if (b >= 0x80u) {
            const std::size_t seqLen = (std::min)(utf8SequenceLength(b), text.size() - i);
            i += seqLen;
            continue;
        }
        const char ch = static_cast<char>(b);
        if (isAsciiUpper(ch)) {
            ++upperCount;
        } else if (isAsciiLower(ch)) {
            ++lowerCount;
        }
        ++i;
    }

    const std::size_t totalLatin = upperCount + lowerCount;
    if (totalLatin == 0 || (upperCount * 100u) <= (totalLatin * 75u)) {
        return std::string(text);
    }

    std::string out;
    out.reserve(text.size());
    bool atSentenceStart = true;

    for (std::size_t i = 0; i < text.size();) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        if (b >= 0x80u) {
            const std::size_t seqLen = (std::min)(utf8SequenceLength(b), text.size() - i);
            const std::string_view seq = text.substr(i, seqLen);
            out.append(seq);
            // Leave Japanese/Chinese/Korean characters completely untouched,
            // and reset sentence start on full-width CJK sentence terminators.
            if (seq == "\xE3\x80\x82" || seq == "\xEF\xBC\x81" || seq == "\xEF\xBC\x9F") {
                atSentenceStart = true;
            }
            i += seqLen;
            continue;
        }

        const char ch = static_cast<char>(b);
        if (isAsciiAlpha(ch)) {
            if (isStandaloneIWord(text, i)) {
                out.push_back('I');
                atSentenceStart = false;
            } else if (atSentenceStart) {
                out.push_back(toUpperAsciiChar(ch));
                atSentenceStart = false;
            } else {
                out.push_back(toLowerAsciiChar(ch));
            }
        } else if (isAsciiDigit(ch)) {
            out.push_back(ch);
            atSentenceStart = false;
        } else if (ch == '.' || ch == '!' || ch == '?') {
            out.push_back(ch);
            atSentenceStart = true;
        } else {
            out.push_back(ch);
        }
        ++i;
    }

    return out;
}

} // namespace

std::string NativeOcrEngine::cleanOcrText(
    const std::string& rawText,
    bool filterSymbols,
    bool normalizeCaps) {
    std::string cleaned = filterSymbols
        ? stripNoiseSymbolsFromText(rawText)
        : rawText;
    if (normalizeCaps) {
        cleaned = normalizeAllCapsToSentenceCase(cleaned);
    }
    return cleaned;
}

std::vector<OcrLanguageInfo> NativeOcrEngine::availableLanguages() {
    std::vector<OcrLanguageInfo> languages;
    ensureWinRtApartment();
    try {
        const auto view = winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages();
        languages.reserve(static_cast<std::size_t>(view.Size()));
        for (const auto& lang : view) {
            OcrLanguageInfo info;
            info.languageTag = winrt::to_string(lang.LanguageTag());
            info.displayName = winrt::to_string(lang.DisplayName());
            languages.push_back(std::move(info));
        }
    } catch (...) {
    }
    return languages;
}

bool NativeOcrEngine::isLanguageSupported(std::string_view languageTag) {
    if (languageTag.empty() || toLowerAscii(languageTag) == "auto") {
        return false;
    }
    ensureWinRtApartment();
    try {
        const winrt::hstring wideTag = winrt::to_hstring(languageTag);
        if (!winrt::Windows::Globalization::Language::IsWellFormed(wideTag)) {
            return false;
        }
        winrt::Windows::Globalization::Language lang(wideTag);
        if (winrt::Windows::Media::Ocr::OcrEngine::IsLanguageSupported(lang)) {
            return true;
        }
        return resolveInstalledLanguage(languageTag).has_value();
    } catch (...) {
        return false;
    }
}

std::uint32_t NativeOcrEngine::maxImageDimension() noexcept {
    ensureWinRtApartment();
    try {
        return winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension();
    } catch (...) {
        return 2600u;
    }
}

Result<OcrResult> NativeOcrEngine::recognizeFrame(
    const ImageFrame& frame,
    std::string_view languageTag,
    std::stop_token stopToken) {
    return recognizeFrame(frame, languageTag, 1.0, stopToken);
}

Result<OcrResult> NativeOcrEngine::recognizeFrame(
    const ImageFrame& frame,
    std::string_view languageTag,
    double upscaleFactor,
    std::stop_token stopToken) {
    return recognizeFrame(frame, languageTag, upscaleFactor, true, true, stopToken);
}

Result<OcrResult> NativeOcrEngine::recognizeFrame(
    const ImageFrame& frame,
    std::string_view languageTag,
    double upscaleFactor,
    bool filterNoiseSymbols,
    bool normalizeAllCaps,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<OcrResult>::err(ErrorCode::Cancelled, "OCR cancelled");
    }

    if (!frame.isValid()) {
        return Result<OcrResult>::err(
            ErrorCode::InvalidArgument, "Cannot run OCR on invalid ImageFrame");
    }

    ensureWinRtApartment();

    try {
        std::string resolvedTag;
        std::string engineErr;
        auto engine = createEngineForLanguage(languageTag, resolvedTag, engineErr);
        if (!engine) {
            return Result<OcrResult>::err(ErrorCode::NotFound, std::move(engineErr));
        }

        if (stopToken.stop_requested()) {
            return Result<OcrResult>::err(ErrorCode::Cancelled, "OCR cancelled");
        }

        const std::uint32_t maxDim = winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension();
        const double clampedUpscale = std::clamp(upscaleFactor, 1.0, 2.0);
        ImageFrame scaledStorage;
        const ImageFrame* targetFrame = &frame;

        if (clampedUpscale > 1.01) {
            std::uint32_t upW = static_cast<std::uint32_t>(
                (std::max)(1L, std::lround(static_cast<double>(frame.width) * clampedUpscale)));
            std::uint32_t upH = static_cast<std::uint32_t>(
                (std::max)(1L, std::lround(static_cast<double>(frame.height) * clampedUpscale)));
            if (maxDim > 0 && (upW > maxDim || upH > maxDim)) {
                const double fitScale = (std::min)(
                    static_cast<double>(maxDim) / static_cast<double>(upW),
                    static_cast<double>(maxDim) / static_cast<double>(upH));
                upW = static_cast<std::uint32_t>(
                    (std::max)(1L, std::lround(static_cast<double>(upW) * fitScale)));
                upH = static_cast<std::uint32_t>(
                    (std::max)(1L, std::lround(static_cast<double>(upH) * fitScale)));
            }
            if (upW != frame.width || upH != frame.height) {
                scaledStorage = ImageDecoder::resizeExact(frame, upW, upH);
                targetFrame = &scaledStorage;
            }
        } else if (maxDim > 0 && (frame.width > maxDim || frame.height > maxDim)) {
            scaledStorage = ImageDecoder::resizeFit(frame, maxDim, maxDim);
            targetFrame = &scaledStorage;
        }

        const std::uint32_t width = targetFrame->width;
        const std::uint32_t height = targetFrame->height;
        const std::vector<std::uint8_t> preprocessedRgba = preprocessFrameForOcr(*targetFrame);

        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(winrt::array_view<const std::uint8_t>(
            preprocessedRgba.data(), preprocessedRgba.data() + preprocessedRgba.size()));
        auto buffer = writer.DetachBuffer();

        auto bitmap = winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
            buffer,
            winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Rgba8,
            static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height),
            winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);

        if (stopToken.stop_requested()) {
            return Result<OcrResult>::err(ErrorCode::Cancelled, "OCR cancelled");
        }

        auto winrtResult = engine.RecognizeAsync(bitmap).get();

        OcrResult out;
        out.languageTag = std::move(resolvedTag);
        out.text = cleanOcrText(
            winrt::to_string(winrtResult.Text()),
            filterNoiseSymbols,
            normalizeAllCaps);
        if (const auto angle = winrtResult.TextAngle()) {
            out.textAngle = angle.Value();
        }

        const auto linesView = winrtResult.Lines();
        out.lines.reserve(static_cast<std::size_t>(linesView.Size()));
        for (const auto& line : linesView) {
            OcrLine outLine;
            outLine.text = cleanOcrText(
                winrt::to_string(line.Text()),
                filterNoiseSymbols,
                normalizeAllCaps);
            if (outLine.text.empty()) {
                continue;
            }
            const auto wordsView = line.Words();
            outLine.words.reserve(static_cast<std::size_t>(wordsView.Size()));
            for (const auto& word : wordsView) {
                OcrWord outWord;
                outWord.text = winrt::to_string(word.Text());
                const auto rect = word.BoundingRect();
                outWord.bounds = OcrBoundingRect{
                    rect.X, rect.Y, rect.Width, rect.Height};
                outLine.words.push_back(std::move(outWord));
            }
            out.lines.push_back(std::move(outLine));
        }

        return Result<OcrResult>::ok(std::move(out));
    } catch (const winrt::hresult_error& ex) {
        return Result<OcrResult>::err(
            ErrorCode::DecodeFailed,
            "Windows OCR error: " + winrt::to_string(ex.message()));
    } catch (const std::exception& ex) {
        return Result<OcrResult>::err(
            ErrorCode::DecodeFailed,
            std::string("Windows OCR exception: ") + ex.what());
    }
}

Result<OcrResult> NativeOcrEngine::recognizeImageBytes(
    std::span<const std::uint8_t> bytes,
    std::string_view languageTag,
    std::stop_token stopToken) {
    if (bytes.empty()) {
        return Result<OcrResult>::err(ErrorCode::InvalidArgument, "Image byte buffer is empty");
    }
    auto frameRes = ImageDecoder::decode(bytes, 0, 0, stopToken);
    if (!frameRes) {
        return Result<OcrResult>::err(frameRes.error());
    }
    return recognizeFrame(frameRes.value(), languageTag, stopToken);
}

Result<OcrResult> NativeOcrEngine::recognizeImageFile(
    const std::filesystem::path& imagePath,
    std::string_view languageTag,
    std::stop_token stopToken) {
    if (imagePath.empty()) {
        return Result<OcrResult>::err(ErrorCode::InvalidArgument, "Image path is empty");
    }

    const std::wstring longPath = FileManager::toWin32LongPath(imagePath);
    std::ifstream ifs(longPath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Result<OcrResult>::err(
            ErrorCode::NotFound,
            "Failed to open image file for OCR: " + FileManager::wideToUtf8(imagePath.native()));
    }

    const std::streamsize fileSize = ifs.tellg();
    if (fileSize <= 0) {
        return Result<OcrResult>::err(
            ErrorCode::DecodeFailed,
            "Image file is empty: " + FileManager::wideToUtf8(imagePath.native()));
    }
    ifs.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    if (!ifs.read(reinterpret_cast<char*>(bytes.data()), fileSize)) {
        return Result<OcrResult>::err(
            ErrorCode::IoError,
            "Failed reading image file for OCR: " + FileManager::wideToUtf8(imagePath.native()));
    }

    return recognizeImageBytes(bytes, languageTag, stopToken);
}

} // namespace mavira::core::imaging
