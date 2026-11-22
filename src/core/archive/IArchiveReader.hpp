#pragma once

#include "core/CommonTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace mavira::core::archive {

/// Pure C++20 interface for reading comic/manga containers (ZIP/CBZ/EPUB, folders, etc.)
/// directly into memory without extracting temporary files to disk.
class IArchiveReader {
public:
    virtual ~IArchiveReader() = default;

    /// Opens the archive or directory at `path` and indexes its image pages.
    [[nodiscard]] virtual mavira::core::Result<void> open(
        const std::filesystem::path& path) = 0;

    /// Closes the container and releases any underlying handles.
    virtual void close() noexcept = 0;

    /// Returns true if a container is currently open.
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;

    /// Returns the total number of indexed image pages.
    [[nodiscard]] virtual std::size_t pageCount() const noexcept = 0;

    /// Returns the logical UTF-8 name of the page at `index`, or empty if out of range.
    [[nodiscard]] virtual std::string pageName(std::size_t index) const = 0;

    /// Reads the raw encoded image bytes for page `index` into memory.
    /// Supports cooperative cancellation via `stopToken`.
    [[nodiscard]] virtual mavira::core::Result<std::vector<std::uint8_t>> readPageBytes(
        std::size_t index,
        std::stop_token stopToken = {}) = 0;

    /// Reads `ComicInfo.xml` metadata if present inside the container.
    /// Returns `ok(std::nullopt)` when the container has no `ComicInfo.xml`.
    [[nodiscard]] virtual mavira::core::Result<std::optional<std::string>> readMetadataXml() = 0;
};

} // namespace mavira::core::archive
