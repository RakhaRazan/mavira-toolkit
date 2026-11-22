#pragma once

#include "core/CommonTypes.hpp"
#include "data/db/DatabaseManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>

namespace mavira::core::cache {

/// Abstraction for the dual-mode cover thumbnail storage engine:
/// - Mode 1 (`RamOnlyCoverProvider`): 0 MB disk bloat, bounded 64 MB RAM LRU.
/// - Mode 2 (`PackFileCoverProvider`): Single-file `<exe_dir>/data/covers.pack` WebP pack.
class ICoverStorage {
public:
    static constexpr std::uint32_t kCoverMaxWidth = 180;
    static constexpr std::uint32_t kCoverMaxHeight = 255;
    static constexpr float kCoverWebpQuality = 75.0f;

    virtual ~ICoverStorage() = default;

    /// Retrieves or decodes the cover thumbnail for `comic`.
    [[nodiscard]] virtual mavira::core::Result<std::shared_ptr<const mavira::core::ImageFrame>>
    getCover(
        const mavira::data::db::ComicRecord& comic,
        std::stop_token stopToken = {}) = 0;

    /// Removes any cached thumbnail for `comicId` from RAM.
    virtual void invalidate(std::int64_t comicId) = 0;

    /// Clears all in-memory cached thumbnails.
    virtual void clearMemoryCache() = 0;

    /// Returns current RAM usage of cached thumbnails in bytes.
    [[nodiscard]] virtual std::size_t memoryUsageBytes() const noexcept = 0;
};

} // namespace mavira::core::cache
