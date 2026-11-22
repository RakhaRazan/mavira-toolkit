#pragma once

#include "core/CommonTypes.hpp"
#include "core/archive/IArchiveReader.hpp"
#include "core/cache/CacheManager.hpp"
#include "core/rendering/RenderingEngine.hpp"
#include "core/threading/WorkerPool.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"

#include <QMetaType>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class QTimer;

Q_DECLARE_METATYPE(mavira::core::rendering::SpreadLayoutResult)

namespace mavira::app {

/// Application-layer controller managing an active comic/manga/PDF reading session.
///
/// Responsibilities:
/// 1. Reading session state (page index, Single/Dual/Vertical Webtoon mode,
///    RTL/LTR direction, Smart Wide-Spread, Auto-Crop, Fit Width/Height/Screen/Original,
///    zoom, rotation, and Named Bookmarks).
/// 2. Asynchronous page decoding via `WorkerPool` + `CacheManager` with 2-page
///    lookahead prefetching (`N+1`, `N+2`) and immediate cooperative cancellation
///    of obsolete tasks on rapid page turns.
/// 3. Debounced SQLite reading progress persistence (`500 ms` default).
class ReaderController : public QObject {
    Q_OBJECT

public:
    static constexpr std::size_t kPrefetchLookahead = 2;
    static constexpr int kDefaultProgressDebounceMs = 500;
    static constexpr float kMinZoomFactor = 0.1f;
    static constexpr float kMaxZoomFactor = 8.0f;
    static constexpr float kZoomStepFactor = 1.25f;

    ReaderController(
        mavira::data::db::DatabaseManager* db,
        mavira::core::cache::CacheManager* cacheManager,
        mavira::core::threading::WorkerPool* workerPool = nullptr,
        QObject* parent = nullptr);

    ~ReaderController() override;

    ReaderController(const ReaderController&) = delete;
    ReaderController& operator=(const ReaderController&) = delete;

    /// Cancels all active decode tasks, flushes pending reading progress to SQLite,
    /// and waits for any in-flight worker tasks to exit cleanly.
    void shutdown();

    // -------------------------------------------------------------------------
    // Session Lifecycle
    // -------------------------------------------------------------------------
    [[nodiscard]] mavira::core::Result<void> openComic(
        const std::filesystem::path& path,
        std::int64_t comicId = 0,
        std::size_t startPage = 0);

    [[nodiscard]] mavira::core::Result<void> openComicRecord(
        const mavira::data::db::ComicRecord& comic);

    /// Opens a session backed by a caller-supplied `IArchiveReader` instance.
    [[nodiscard]] mavira::core::Result<void> openReader(
        std::unique_ptr<mavira::core::archive::IArchiveReader> reader,
        std::int64_t comicId = 0,
        std::size_t startPage = 0,
        const std::filesystem::path& sourcePath = {});

    void closeComic();

    /// If the currently open comic matches `comicId` or `filePath`, immediately releases
    /// its `IArchiveReader` handle, evicts its cached pages from `CacheManager`, clears
    /// active reader state, and emits `activeComicClosed()`.
    bool closeIfMatching(std::int64_t comicId, const std::wstring& filePath);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] std::int64_t comicId() const noexcept { return comicId_; }
    [[nodiscard]] std::size_t totalPages() const noexcept { return totalPages_; }
    [[nodiscard]] std::size_t currentPage() const noexcept { return currentPage_; }
    [[nodiscard]] const std::filesystem::path& currentPath() const noexcept { return currentPath_; }

    // -------------------------------------------------------------------------
    // Navigation
    // -------------------------------------------------------------------------
    bool goToPage(std::size_t pageIndex);
    bool nextPage();
    bool previousPage();
    bool firstPage();
    bool lastPage();

    // -------------------------------------------------------------------------
    // View & Layout State
    // -------------------------------------------------------------------------
    void applySettings(const mavira::data::config::AppSettings& settings);

    void setReadingMode(mavira::core::rendering::ReadingMode mode);
    [[nodiscard]] mavira::core::rendering::ReadingMode readingMode() const noexcept {
        return readingMode_;
    }

    void setReadingDirection(mavira::core::rendering::ReadingDirection direction);
    [[nodiscard]] mavira::core::rendering::ReadingDirection readingDirection() const noexcept {
        return readingDirection_;
    }

    void setFitMode(mavira::core::rendering::FitMode mode);
    [[nodiscard]] mavira::core::rendering::FitMode fitMode() const noexcept {
        return fitMode_;
    }

    void setSmartWideSpread(bool enabled);
    [[nodiscard]] bool smartWideSpread() const noexcept { return smartWideSpread_; }

    void setAutoCropBorders(bool enabled);
    [[nodiscard]] bool autoCropBorders() const noexcept { return autoCropBorders_; }

    void setZoomFactor(float zoom);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    [[nodiscard]] float zoomFactor() const noexcept { return zoomFactor_; }

    void setRotation(int degrees);
    void rotateClockwise();
    void rotateCounterClockwise();
    [[nodiscard]] int rotationDegrees() const noexcept { return rotationDegrees_; }

    [[nodiscard]] mavira::core::rendering::LayoutOptions currentLayoutOptions() const noexcept;

    // -------------------------------------------------------------------------
    // Named Bookmarks
    // -------------------------------------------------------------------------
    [[nodiscard]] mavira::core::Result<std::int64_t> addBookmark(
        std::string_view name,
        std::optional<std::size_t> pageIndex = std::nullopt);

    [[nodiscard]] mavira::core::Result<void> removeBookmark(std::int64_t bookmarkId);

    [[nodiscard]] const std::vector<mavira::data::db::BookmarkRecord>& bookmarks() const noexcept {
        return bookmarks_;
    }

    bool goToBookmark(std::int64_t bookmarkId);

    // -------------------------------------------------------------------------
    // Progress Persistence & Diagnostics
    // -------------------------------------------------------------------------
    void setProgressDebounceMs(int ms);
    [[nodiscard]] int progressDebounceMs() const noexcept { return progressDebounceMs_; }

    /// Immediately persists any pending reading progress update to SQLite.
    void flushReadingProgress();

    [[nodiscard]] bool isPageCached(std::size_t pageIndex) const;
    [[nodiscard]] std::size_t activeTaskCount() const;
    [[nodiscard]] std::uint64_t cancelledTaskCount() const noexcept {
        return cancelledTaskCount_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t decodedPageCount() const noexcept {
        return decodedPageCount_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::optional<mavira::core::rendering::SpreadLayoutResult> currentLayout() const {
        return currentLayout_;
    }

signals:
    void comicOpened(qint64 comicId, int totalPages);
    void comicClosed();
    void activeComicClosed();
    void pageChanged(int pageIndex, int totalPages);
    void pageReady(int pageIndex, const mavira::core::rendering::SpreadLayoutResult& layout);
    void pagePrefetched(int pageIndex);
    void pageTaskCancelled(int pageIndex);
    void viewStateChanged();
    void bookmarksChanged();
    void readingProgressSaved(qint64 comicId, int pageIndex, bool isRead);
    void errorOccurred(const QString& message);

private:
    struct ReaderSession {
        std::unique_ptr<mavira::core::archive::IArchiveReader> reader{};
        std::mutex ioMutex{};
        bool isPdf{false};
    };

    struct InFlightTask {
        mavira::core::threading::TaskHandle handle{};
        std::uint64_t sessionGeneration{0};
        bool isPrefetch{false};
    };

    [[nodiscard]] std::int64_t effectiveCacheComicId() const noexcept;
    [[nodiscard]] std::vector<std::size_t> computeDesiredPages(std::size_t basePage) const;

    void requestCurrentAndPrefetchPages();
    void enqueuePageDecode(
        std::size_t pageIndex,
        mavira::core::RequestPriority priority,
        bool isPrefetch);
    void cancelAllInFlightTasks();
    void tryComposeAndEmitCurrentPage();
    void scheduleProgressSave();
    void loadBookmarks();

    mavira::data::db::DatabaseManager* db_{nullptr};
    std::unique_ptr<mavira::core::cache::CacheManager> ownedCache_{};
    mavira::core::cache::CacheManager* cacheManager_{nullptr};
    std::unique_ptr<mavira::core::threading::WorkerPool> ownedPool_{};
    mavira::core::threading::WorkerPool* pool_{nullptr};

    QTimer* progressDebounceTimer_{nullptr};
    int progressDebounceMs_{kDefaultProgressDebounceMs};
    bool progressDirty_{false};

    std::shared_ptr<ReaderSession> session_{};
    std::filesystem::path currentPath_{};
    std::int64_t comicId_{0};
    std::int64_t syntheticCacheId_{0};
    std::size_t totalPages_{0};
    std::size_t currentPage_{0};
    std::size_t lastPagesConsumed_{1};
    std::uint64_t sessionGeneration_{0};

    mavira::core::rendering::ReadingMode readingMode_{mavira::core::rendering::ReadingMode::SinglePage};
    mavira::core::rendering::ReadingDirection readingDirection_{mavira::core::rendering::ReadingDirection::RTL};
    mavira::core::rendering::FitMode fitMode_{mavira::core::rendering::FitMode::FitScreen};
    bool smartWideSpread_{true};
    bool autoCropBorders_{false};
    float zoomFactor_{1.0f};
    int rotationDegrees_{0};

    std::optional<mavira::core::rendering::SpreadLayoutResult> currentLayout_{};
    std::vector<mavira::data::db::BookmarkRecord> bookmarks_{};
    std::int64_t nextSyntheticBookmarkId_{-1};

    std::unordered_map<std::size_t, InFlightTask> inflightTasks_{};
    std::atomic<std::uint64_t> cancelledTaskCount_{0};
    std::atomic<std::uint64_t> decodedPageCount_{0};

    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};
    std::atomic<bool> shuttingDown_{false};
    mutable std::mutex workerWaitMutex_{};
    std::condition_variable workerWaitCv_{};
    std::size_t runningWorkers_{0};
};

} // namespace mavira::app
