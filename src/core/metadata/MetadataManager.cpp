#include "core/metadata/MetadataManager.hpp"

#include "data/fs/FileManager.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace mavira::core::metadata {

namespace {

using mavira::core::ErrorCode;
using mavira::core::Result;
using mavira::core::archive::PdfMetadata;
using mavira::data::fs::FileManager;

[[nodiscard]] std::string trim(std::string_view sv) {
    std::size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start])) != 0) {
        ++start;
    }
    std::size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1])) != 0) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

[[nodiscard]] std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] std::string decodeXmlEntities(std::string_view input) {
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') {
            out.push_back(input[i]);
            continue;
        }
        const std::size_t semi = input.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back(input[i]);
            continue;
        }

        const std::string_view entity = input.substr(i + 1, semi - i - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (entity.size() >= 2 && entity[0] == '#') {
            unsigned int codepoint = 0;
            bool ok = false;
            if (entity[1] == 'x' || entity[1] == 'X') {
                const auto res = std::from_chars(
                    entity.data() + 2, entity.data() + entity.size(), codepoint, 16);
                ok = (res.ec == std::errc{} && res.ptr == entity.data() + entity.size());
            } else {
                const auto res = std::from_chars(
                    entity.data() + 1, entity.data() + entity.size(), codepoint, 10);
                ok = (res.ec == std::errc{} && res.ptr == entity.data() + entity.size());
            }
            if (ok && codepoint > 0 && codepoint <= 0x7F) {
                out.push_back(static_cast<char>(codepoint));
            } else {
                out.append(input.substr(i, semi - i + 1));
            }
        } else {
            out.append(input.substr(i, semi - i + 1));
        }
        i = semi;
    }

    return out;
}

[[nodiscard]] std::optional<std::string> extractXmlTagValue(
    std::string_view xml,
    std::string_view tagName) {
    // Case-insensitive search for <TagName ...>...</TagName>
    const std::string lowerXml = toLowerAscii(xml);
    const std::string openPrefix = "<" + toLowerAscii(tagName);
    const std::string closeTag = "</" + toLowerAscii(tagName) + ">";

    std::size_t pos = 0;
    while ((pos = lowerXml.find(openPrefix, pos)) != std::string::npos) {
        const std::size_t afterName = pos + openPrefix.size();
        if (afterName >= lowerXml.size()) {
            break;
        }
        const char nextCh = lowerXml[afterName];
        if (nextCh != '>' && nextCh != '/' && std::isspace(static_cast<unsigned char>(nextCh)) == 0) {
            pos = afterName;
            continue;
        }

        const std::size_t tagEnd = lowerXml.find('>', afterName);
        if (tagEnd == std::string::npos) {
            break;
        }

        // Self-closing tag <Tag/>
        if (tagEnd > afterName && lowerXml[tagEnd - 1] == '/') {
            return std::string{};
        }

        const std::size_t closePos = lowerXml.find(closeTag, tagEnd + 1);
        if (closePos == std::string::npos) {
            break;
        }

        std::string_view rawInner = xml.substr(tagEnd + 1, closePos - (tagEnd + 1));
        constexpr std::string_view kCdataStart = "<![CDATA[";
        constexpr std::string_view kCdataEnd = "]]>";
        if (rawInner.starts_with(kCdataStart) && rawInner.ends_with(kCdataEnd) &&
            rawInner.size() >= kCdataStart.size() + kCdataEnd.size()) {
            rawInner = rawInner.substr(
                kCdataStart.size(),
                rawInner.size() - kCdataStart.size() - kCdataEnd.size());
            return trim(rawInner);
        }

        return trim(decodeXmlEntities(rawInner));
    }

    return std::nullopt;
}

void appendSplitList(std::vector<std::string>& dst, std::string_view csv) {
    std::size_t start = 0;
    while (start <= csv.size()) {
        const std::size_t delim = csv.find_first_of(",;", start);
        const std::string_view token = (delim == std::string_view::npos)
            ? csv.substr(start)
            : csv.substr(start, delim - start);
        const std::string cleaned = trim(token);
        if (!cleaned.empty()) {
            const std::string lowerCleaned = toLowerAscii(cleaned);
            const bool exists = std::any_of(dst.begin(), dst.end(), [&](const std::string& item) {
                return toLowerAscii(item) == lowerCleaned;
            });
            if (!exists) {
                dst.push_back(cleaned);
            }
        }
        if (delim == std::string_view::npos) {
            break;
        }
        start = delim + 1;
    }
}

[[nodiscard]] std::optional<int> parseOptionalInt(std::string_view text) {
    const std::string cleaned = trim(text);
    if (cleaned.empty()) {
        return std::nullopt;
    }
    int value = 0;
    const auto res = std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value);
    if (res.ec == std::errc{}) {
        return value;
    }
    return std::nullopt;
}

[[nodiscard]] std::string stripKnownArchiveExtension(std::string_view input) {
    const std::size_t dotPos = input.find_last_of('.');
    if (dotPos == std::string_view::npos || dotPos == 0) {
        return std::string(input);
    }
    const std::string ext = toLowerAscii(input.substr(dotPos));
    if (ext == ".cbz" || ext == ".zip" || ext == ".cbr" || ext == ".rar" ||
        ext == ".cb7" || ext == ".7z" || ext == ".cbt" || ext == ".tar" ||
        ext == ".epub" || ext == ".pdf") {
        return std::string(input.substr(0, dotPos));
    }
    return std::string(input);
}

[[nodiscard]] std::string collapseSpacesAndSeparators(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool prevSpace = false;
    for (char ch : input) {
        if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!prevSpace && !out.empty()) {
                out.push_back(' ');
                prevSpace = true;
            }
        } else {
            out.push_back(ch);
            prevSpace = false;
        }
    }

    // Trim trailing spaces and stray separator punctuation left by stripped tags.
    while (!out.empty() &&
           (std::isspace(static_cast<unsigned char>(out.back())) != 0 ||
            out.back() == '-' || out.back() == '_')) {
        out.pop_back();
    }
    std::size_t lead = 0;
    while (lead < out.size() &&
           (std::isspace(static_cast<unsigned char>(out[lead])) != 0 ||
            out[lead] == '-' || out[lead] == '_')) {
        ++lead;
    }
    if (lead > 0) {
        out.erase(0, lead);
    }
    return out;
}

} // namespace

Result<ComicMetadata> MetadataManager::parseComicInfoXml(std::string_view xmlContent) {
    if (xmlContent.empty()) {
        return Result<ComicMetadata>::err(ErrorCode::InvalidArgument, "ComicInfo.xml content is empty");
    }

    const std::string lowerXml = toLowerAscii(xmlContent);
    if (lowerXml.find("<comicinfo") == std::string::npos) {
        return Result<ComicMetadata>::err(
            ErrorCode::DecodeFailed, "XML does not contain a <ComicInfo> root element");
    }

    ComicMetadata meta;
    if (auto title = extractXmlTagValue(xmlContent, "Title"); title && !title->empty()) {
        meta.title = std::move(*title);
    }
    if (auto series = extractXmlTagValue(xmlContent, "Series"); series && !series->empty()) {
        meta.series = std::move(*series);
    }
    if (auto writer = extractXmlTagValue(xmlContent, "Writer"); writer && !writer->empty()) {
        meta.writer = std::move(*writer);
    } else if (auto penciller = extractXmlTagValue(xmlContent, "Penciller");
               penciller && !penciller->empty()) {
        meta.writer = std::move(*penciller);
    }
    if (auto summary = extractXmlTagValue(xmlContent, "Summary"); summary && !summary->empty()) {
        meta.summary = std::move(*summary);
    }
    if (auto genre = extractXmlTagValue(xmlContent, "Genre"); genre && !genre->empty()) {
        appendSplitList(meta.genres, *genre);
    }
    if (auto tags = extractXmlTagValue(xmlContent, "Tags"); tags && !tags->empty()) {
        appendSplitList(meta.tags, *tags);
    }
    if (auto vol = extractXmlTagValue(xmlContent, "Volume"); vol && !vol->empty()) {
        meta.volumeNumber = parseOptionalInt(*vol);
    } else if (auto num = extractXmlTagValue(xmlContent, "Number"); num && !num->empty()) {
        meta.volumeNumber = parseOptionalInt(*num);
    }
    if (auto pages = extractXmlTagValue(xmlContent, "PageCount"); pages && !pages->empty()) {
        meta.pageCount = parseOptionalInt(*pages);
    }

    return Result<ComicMetadata>::ok(std::move(meta));
}

void MetadataManager::mergePdfMetadata(ComicMetadata& target, const PdfMetadata& pdfMeta) {
    const std::string title = trim(pdfMeta.title);
    if (target.title.empty() && !title.empty()) {
        target.title = title;
    }

    const std::string author = trim(pdfMeta.author);
    if (target.writer.empty() && !author.empty()) {
        target.writer = author;
    }

    const std::string subject = trim(pdfMeta.subject);
    if (!subject.empty()) {
        if (target.summary.empty()) {
            target.summary = subject;
        }
        if (target.series.empty()) {
            target.series = subject;
        }
    }

    if (!pdfMeta.keywords.empty()) {
        appendSplitList(target.tags, pdfMeta.keywords);
    }
}

ComicMetadata MetadataManager::parseFromFilename(const std::filesystem::path& path) {
    ComicMetadata meta;

    const std::wstring stemWide = path.has_stem()
        ? path.stem().native()
        : path.filename().native();
    const std::string stem = FileManager::wideToUtf8(stemWide);
    if (stem.empty()) {
        return meta;
    }

    // Pattern 1: Optional leading (Event), [Author], Title, optional trailing (Event) / [Tags]
    // e.g. "[Author Name] My Comic Title (C103)" or "(C103) [Author Name] My Comic Title [EN]"
    static const std::regex kAuthorTitleEventRegex(
        R"(^(?:\(([^)]+)\)\s*)?\[([^\]]+)\]\s*(.+?)(?:\s*\(([^)]+)\))?(?:\s*\[[^\]]+\])*$)");

    std::smatch match;
    if (std::regex_match(stem, match, kAuthorTitleEventRegex)) {
        const std::string leadingEvent = trim(match[1].str());
        const std::string author = trim(match[2].str());
        const std::string title = trim(match[3].str());
        const std::string trailingEvent = trim(match[4].str());

        meta.writer = author;
        meta.event = !trailingEvent.empty() ? trailingEvent : leadingEvent;
        meta.title = cleanDisplayTitle(title);
    } else {
        meta.title = cleanDisplayTitle(stem);
    }

    // Extract optional volume number (e.g., "Vol. 3", "Vol 03", "v12")
    static const std::regex kVolumeRegex(R"((?:^|[\s_\-])(?:vol\.?|v)\s*0*(\d+)(?:\b|$))",
                                         std::regex_constants::icase);
    std::smatch volMatch;
    if (std::regex_search(stem, volMatch, kVolumeRegex)) {
        meta.volumeNumber = parseOptionalInt(volMatch[1].str());
    }

    meta.displayTitle = meta.title;
    return meta;
}

std::string MetadataManager::cleanDisplayTitle(std::string_view rawTitle) {
    std::string work = stripKnownArchiveExtension(rawTitle);

    // Strip bracketed/parenthesized/braced metadata blocks: [...], (...), {...}
    static const std::regex kBracketTagRegex(R"(\[[^\]]*\]|\([^)]*\)|\{[^}]*\})");
    work = trim(std::regex_replace(work, kBracketTagRegex, " "));

    // Replace dots used as word separators (when no spaces are present in the remaining title).
    if (work.find(' ') == std::string::npos && work.find('.') != std::string::npos) {
        std::replace(work.begin(), work.end(), '.', ' ');
    }

    work = collapseSpacesAndSeparators(work);
    if (work.empty()) {
        return collapseSpacesAndSeparators(stripKnownArchiveExtension(rawTitle));
    }
    return work;
}

ComicMetadata MetadataManager::extractMetadata(
    const std::filesystem::path& path,
    const std::optional<std::string>& comicInfoXml,
    const std::optional<PdfMetadata>& pdfMeta,
    bool smartRename) {
    ComicMetadata result;

    if (comicInfoXml.has_value() && !comicInfoXml->empty()) {
        auto xmlRes = parseComicInfoXml(*comicInfoXml);
        if (xmlRes.isOk()) {
            result = std::move(xmlRes).value();
        }
    }

    if (pdfMeta.has_value()) {
        mergePdfMetadata(result, *pdfMeta);
    }

    const ComicMetadata filenameMeta = parseFromFilename(path);
    if (result.title.empty()) {
        result.title = filenameMeta.title;
    }
    if (result.writer.empty()) {
        result.writer = filenameMeta.writer;
    }
    if (result.event.empty()) {
        result.event = filenameMeta.event;
    }
    if (!result.volumeNumber.has_value()) {
        result.volumeNumber = filenameMeta.volumeNumber;
    }

    const std::string rawStem = FileManager::wideToUtf8(
        path.has_stem() ? path.stem().native() : path.filename().native());
    const std::string& baseTitle = result.title.empty() ? rawStem : result.title;
    result.displayTitle = smartRename ? cleanDisplayTitle(baseTitle) : baseTitle;

    return result;
}

} // namespace mavira::core::metadata
