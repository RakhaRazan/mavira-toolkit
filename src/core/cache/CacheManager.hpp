#pragma once

#include "core/CommonTypes.hpp"
#include "core/cache/ICoverStorage.hpp"
#include "data/db/DatabaseManager.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_map>

namespace mavira::core::cache {

struct PageCacheKey {
    std::int64_t comicId{0};
    std::size_t pageIndex{0};

    [[nodiscard]] bool operator==(const PageCacheKey& other) const noexcept = default;
};

struct PageCacheKeyHash {
    [[nodiscard]] std::size_t operator()(const PageCacheKey& k) const noexcept {
        const std::size_t h1 = std::hash<std::int64_t>{}(k.comicId);
        const std::size_t h2 = std::hash<std::size_t>{}(k.pageIndex);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

/// L1 Bounded Byte-Budget RAM LRU Cache for active reader pages.
/// Evicts pages furthest from the current reading position first (tie-broken by LRU).
class PageRamCache {
public:
    static constexpr std::size_t kDefaultLimitBytes = 128ULL * 1024ULL * 1024ULL; // 128 MB

    explicit PageRamCache(std::size_t maxBytes = kDefaultLimitBytes);

    void setMemoryLimitBytes(std::size_t maxBytes);
    [[nodiscard]] std::size_t memoryLimitBytes() const noexcept;
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept;
    [[nodiscard]] std::size_t entryCount() const noexcept;

    /// Updates the reader's active comic & page index used to prioritize eviction distance.
    /// When `isSequentialArchive` is true (e.g., RAR5/7z), protects a backward retention
    /// window (`N-5 .. N+2`) to avoid expensive stream reopen/re-decompression on back flips.
    void setCurrentReadingPosition(
        std::int64_t comicId,
        std::size_t pageIndex,
        bool isSequentialArchive = false) noexcept;

    /// Inserts or updates a decoded page frame, evicting furthest pages until within budget.
    void put(const PageCacheKey& key, std::shared_ptr<const mavira::core::ImageFrame> frame);

    /// Looks up a cached page and refreshes its LRU timestamp.
    [[nodiscard]] std::shared_ptr<const mavira::core::ImageFrame> get(const PageCacheKey& key);

    [[nodiscard]] bool contains(const PageCacheKey& key) const;
    void erase(const PageCacheKey& key);
    void eraseComic(std::int64_t comicId);
    void clear();

private:
    struct Entry {
        std::shared_ptr<const mavira::core::ImageFrame> frame{};
        std::size_t bytes{0};
        std::uint64_t lastAccessTick{0};
    };

    void evictIfNeededLocked();

    mutable std::mutex mutex_{};
    std::size_t maxBytes_{kDefaultLimitBytes};
    std::size_t usedBytes_{0};
    std::int64_t currentComicId_{0};
    std::size_t currentPageIndex_{0};
    bool currentComicSequential_{false};
    std::uint64_t accessTick_{0};
    std::unordered_map<PageCacheKey, Entry, PageCacheKeyHash> entries_{};
};

/// Bounded byte-budget RAM LRU helper keyed by `comicId`.
class CoverRamLru {
public:
    explicit CoverRamLru(std::size_t maxBytes);

    void put(std::int64_t comicId, std::shared_ptr<const mavira::core::ImageFrame> frame);
    [[nodiscard]] std::shared_ptr<const mavira::core::ImageFrame> get(std::int64_t comicId);
    void erase(std::int64_t comicId);
    void clear();
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept;
    [[nodiscard]] std::size_t entryCount() const noexcept;

private:
    struct Node {
        std::int64_t comicId{0};
        std::shared_ptr<const mavira::core::ImageFrame> frame{};
        std::size_t bytes{0};
    };

    mutable std::mutex mutex_{};
    std::size_t maxBytes_{0};
    std::size_t usedBytes_{0};
    std::list<Node> lruList_{};
    std::unordered_map<std::int64_t, std::list<Node>::iterator> index_{};
};

/// Mode 1 Cover Provider (0 MB Disk): extracts cover thumbnails on demand into a
/// bounded 64 MB RAM LRU cache with instant `std::stop_token` cancellation on fast scroll.
class RamOnlyCoverProvider final : public ICoverStorage {
public:
    static constexpr std::size_t kDefaultRamBudgetBytes = 64ULL * 1024ULL * 1024ULL; // 64 MB

    explicit RamOnlyCoverProvider(std::size_t maxRamBytes = kDefaultRamBudgetBytes);

    [[nodiscard]] mavira::core::Result<std::shared_ptr<const mavira::core::ImageFrame>>
    getCover(
        const mavira::data::db::ComicRecord& comic,
        std::stop_token stopToken = {}) override;

    void invalidate(std::int64_t comicId) override;
    void clearMemoryCache() override;
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept override;

private:
    CoverRamLru ramLru_;
};

/// Mode 2 Cover Provider (Single-File `covers.pack`): sequentially indexes uncached covers
/// (`ORDER BY file_path ASC`), encodes compact WebP thumbnails (`180x255`, quality 75),
/// appends them to `<exe_dir>/data/covers.pack`, and batch-flushes stream + SQLite
/// `(cover_pack_offset, cover_pack_size)` every 50 covers.
class PackFileCoverProvider final : public ICoverStorage {
public:
    static constexpr std::size_t kBatchFlushInterval = 50;
    static constexpr std::size_t kDefaultRamBudgetBytes = 32ULL * 1024ULL * 1024ULL; // 32 MB

    PackFileCoverProvider(
        std::filesystem::path packFilePath,
        mavira::data::db::DatabaseManager* db,
        std::size_t maxRamBytes = kDefaultRamBudgetBytes);
    ~PackFileCoverProvider() override;

    void closePackHandle() noexcept;

    [[nodiscard]] mavira::core::Result<std::shared_ptr<const mavira::core::ImageFrame>>
    getCover(
        const mavira::data::db::ComicRecord& comic,
        std::stop_token stopToken = {}) override;

    void invalidate(std::int64_t comicId) override;
    void clearMemoryCache() override;
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept override;

    /// Reads raw WebP bytes directly from `covers.pack` at `[offset, offset + size)`.
    [[nodiscard]] mavira::core::Result<std::vector<std::uint8_t>> readRawWebpFromPack(
        std::int64_t offset,
        std::uint32_t size,
        std::stop_token stopToken = {}) const;

    /// Sequentially indexes all uncached covers from SQLite (`ORDER BY file_path ASC`),
    /// appending WebP thumbnails to `covers.pack` and flushing file + DB every 50 covers.
    /// Returns the number of newly indexed covers.
    [[nodiscard]] mavira::core::Result<std::size_t> indexUncachedCovers(
        std::stop_token stopToken = {},
        std::function<void(std::size_t indexed, std::size_t total)> progressCb = {});

    [[nodiscard]] const std::filesystem::path& packFilePath() const noexcept {
        return packFilePath_;
    }

private:
    std::filesystem::path packFilePath_{};
    mavira::data::db::DatabaseManager* db_{nullptr};
    mutable std::mutex packIoMutex_{};
    mutable void* packHandle_{nullptr};
    CoverRamLru ramLru_;
};

/// Top-level facade owning the L1 Page RAM Cache and the active Cover Storage provider.
class CacheManager {
public:
    CacheManager(
        std::size_t pageRamLimitBytes = PageRamCache::kDefaultLimitBytes,
        std::unique_ptr<ICoverStorage> coverProvider = std::make_unique<RamOnlyCoverProvider>());

    [[nodiscard]] PageRamCache& pageCache() noexcept { return pageCache_; }
    [[nodiscard]] const PageRamCache& pageCache() const noexcept { return pageCache_; }

    [[nodiscard]] ICoverStorage* coverStorage() noexcept { return coverProvider_.get(); }
    [[nodiscard]] const ICoverStorage* coverStorage() const noexcept { return coverProvider_.get(); }

    void setCoverStorage(std::unique_ptr<ICoverStorage> provider);

private:
    PageRamCache pageCache_;
    std::unique_ptr<ICoverStorage> coverProvider_;
};

} // namespace mavira::core::cache
