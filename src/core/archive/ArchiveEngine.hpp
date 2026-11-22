#pragma once

#include "core/CommonTypes.hpp"
#include "core/archive/IArchiveReader.hpp"

#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

namespace mavira::core::archive {

/// Detects container formats (via magic bytes with extension fallback) and constructs
/// the appropriate `IArchiveReader` implementation.
class ArchiveEngine {
public:
    /// Detects the comic container format by inspecting directory status, file magic bytes,
    /// and falling back to extension-based detection.
    [[nodiscard]] static mavira::core::ComicFormat detectFormat(
        const std::filesystem::path& path);

    /// Creates an unopened reader instance for the specified `format`.
    [[nodiscard]] static mavira::core::Result<std::unique_ptr<IArchiveReader>> createReader(
        mavira::core::ComicFormat format);

    /// Detects the container format at `path`, creates the matching reader, opens it,
    /// and returns the ready-to-use reader.
    [[nodiscard]] static mavira::core::Result<std::unique_ptr<IArchiveReader>> openArchive(
        const std::filesystem::path& path);

    /// Extracts all pages from `archivePath` into `outputDir`, returning the number of extracted files.
    [[nodiscard]] static mavira::core::Result<std::size_t> extractPagesToDirectory(
        const std::filesystem::path& archivePath,
        const std::filesystem::path& outputDir,
        std::stop_token stopToken = {});

    /// Merges one or more comic containers into a single `.cbz` archive at `outputCbzPath`
    /// by streaming one page at a time to keep peak RAM flat.
    [[nodiscard]] static mavira::core::Result<std::size_t> mergeComicsToCbz(
        const std::vector<std::filesystem::path>& inputPaths,
        const std::filesystem::path& outputCbzPath,
        std::stop_token stopToken = {});
};

} // namespace mavira::core::archive
