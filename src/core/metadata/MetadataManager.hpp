#pragma once

#include "core/CommonTypes.hpp"
#include "core/archive/PdfDocumentReader.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mavira::core::metadata {

struct ComicMetadata {
    std::string title{};
    std::string displayTitle{};
    std::string series{};
    std::string writer{};
    std::string event{};
    std::string summary{};
    std::vector<std::string> genres{};
    std::vector<std::string> tags{};
    std::optional<int> volumeNumber{};
    std::optional<int> pageCount{};
};

/// Pure C++20 metadata parser & normalizer supporting `ComicInfo.xml`, PDF Info
/// dictionaries, regex filename fallback (`[Author] Title (Event)`), and Smart Rename
/// visual title cleaning.
class MetadataManager {
public:
    /// Parses `ComicInfo.xml` content (`Title`, `Series`, `Writer`, `Tags`, `Genre`, etc.).
    [[nodiscard]] static mavira::core::Result<ComicMetadata> parseComicInfoXml(
        std::string_view xmlContent);

    /// Merges PDF Info dictionary metadata (`Title`, `Author`, `Subject`, `Keywords`)
    /// into `target`, filling missing fields and appending deduplicated tags.
    static void mergePdfMetadata(
        ComicMetadata& target,
        const mavira::core::archive::PdfMetadata& pdfMeta);

    /// Parses fallback metadata from a filename/path using regex patterns such as
    /// `[Author] Title (Event)` or `(Event) [Author] Title`.
    [[nodiscard]] static ComicMetadata parseFromFilename(const std::filesystem::path& path);

    /// Smart Rename visual title cleaner: strips extensions, bracketed/parenthesized
    /// release tags (`[...]`, `(...)`, `{...}`), normalizes underscores/dots, and trims.
    [[nodiscard]] static std::string cleanDisplayTitle(std::string_view rawTitle);

    /// Full metadata extraction pipeline combining `ComicInfo.xml`, optional PDF metadata,
    /// filename regex fallback, and optional Smart Rename display title cleaning.
    [[nodiscard]] static ComicMetadata extractMetadata(
        const std::filesystem::path& path,
        const std::optional<std::string>& comicInfoXml = std::nullopt,
        const std::optional<mavira::core::archive::PdfMetadata>& pdfMeta = std::nullopt,
        bool smartRename = true);
};

} // namespace mavira::core::metadata
