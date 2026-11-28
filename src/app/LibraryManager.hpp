#pragma once

#include "core/CommonTypes.hpp"
#include "core/cache/CacheManager.hpp"
#include "core/threading/WorkerPool.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

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
#include <unordered_map>
#include <vector>

class QTimer;

namespace mavira::app {

/// Describes a detected Windows cloud-sync root folder (OneDrive, Google Drive, Dropbox).
struct CloudFolderInfo {
    std::string provider{};
    std::filesystem::path path{};
};

/// Application-layer coordinator connecting FileManager, MetadataManager,
/// DatabaseManager, and CacheManager.
///
/// Responsibilities:
/// 1. Background recursive folder scanning with 500-item batched SQLite commits
///    and queued Qt progress/update signals (never blocking the UI thread).
/// 2. Live folder watching via Win32 ReadDirectoryChangesW for automatic incremental
///    refresh while enforcing the Unplugged Drive Guard.
/// 3. Auto-detection of Windows cloud sync roots and background Offline Pinning
///    into `<exe_dir>/data/offline_cache/`.
class LibraryManager : public QObject {
    Q_OBJECT

public:
    static constexpr std::size_t kCommitBatchSize = 500;
    static constexpr int kDefaultDebounceMs = 400;
    static constexpr int kDefaultOrphanPruneIntervalMs = 60000;

    LibraryManager(
        mavira::data::db::DatabaseManager* db,
        mavira::core::cache::CacheManager* cacheManager,
        mavira::data::fs::PortablePaths portablePaths = mavira::data::fs::FileManager::resolvePortablePaths(),
        mavira::core::threading::WorkerPool* workerPool = nullptr,
        QObject* parent = nullptr);

    ~LibraryManager() override;

    LibraryManager(const LibraryManager&) = delete;
    LibraryManager& operator=(const LibraryManager&) = delete;

    /// Cancels all active background tasks, stops all Win32 folder watchers, and waits
    /// for in-flight workers to finish cleanly. Safe to call multiple times.
    void shutdown();

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------
    void setSmartRename(bool enabled) noexcept;
    [[nodiscard]] bool smartRename() const noexcept;

    void setDebounceIntervalMs(int ms) noexcept;
    [[nodiscard]] int debounceIntervalMs() const noexcept;

    [[nodiscard]] const mavira::data::fs::PortablePaths& portablePaths() const noexcept {
        return portablePaths_;
    }

    // -------------------------------------------------------------------------
    // Watched Folders & Background Scanning
    // -------------------------------------------------------------------------
    /// Registers a watched folder in SQLite (capturing its persistent Volume GUID),
    /// starts watching it if watchers are active, and optionally triggers an async scan.
    [[nodiscard]] mavira::core::Result<std::int64_t> addWatchedFolder(
        const std::filesystem::path& folderPath,
        bool triggerScan = true);

    /// Removes a watched folder from SQLite and stops watching it.
    [[nodiscard]] mavira::core::Result<void> removeWatchedFolder(std::int64_t folderId);

    /// Removes a comic from SQLite (`comics`, `comics_fts`, `bookmarks`, `shelf_items`),
    /// optionally moves the file/folder to the Windows Recycle Bin via `SHFileOperationW`,
    /// cleans up any cached cover/offline-pinned copy, and immediately emits `libraryUpdated()`.
    [[nodiscard]] mavira::core::Result<void> removeComicFromLibrary(
        std::int64_t comicId,
        bool deleteFromDisk = false);

    /// Runs an asynchronous orphan-prune pass across standalone and watched-folder comics
    /// while respecting the Unplugged Drive Guard.
    void pruneOrphansAsync();

    /// Scans all enabled watched folders in the background.
    void scanAllFoldersAsync();

    /// Scans a single folder (and its nested subdirectories) in the background.
    void scanFolderAsync(
        const std::filesystem::path& folderPath,
        std::optional<std::int64_t> watchedFolderId = std::nullopt);

    /// Requests cooperative cancellation of any currently running background scan.
    void cancelActiveScan();

    [[nodiscard]] bool isScanning() const noexcept;

    /// If CacheManager uses PackFileCoverProvider (Mode 2), schedules background
    /// sequential cover indexing into `<exe_dir>/data/covers.pack`.
    void indexCoversAsync();

    // -------------------------------------------------------------------------
    // Win32 ReadDirectoryChangesW Live Folder Watchers
    // -------------------------------------------------------------------------
    /// Starts overlapped Win32 ReadDirectoryChangesW watchers for all enabled watched folders.
    void startWatching();

    /// Stops all active Win32 folder watchers cleanly.
    void stopWatching();

    [[nodiscard]] bool isWatching() const noexcept;
    [[nodiscard]] std::size_t activeWatcherCount() const;

    // -------------------------------------------------------------------------
    // Cloud Sync Detection & Background Offline Pinning
    // -------------------------------------------------------------------------
    /// Auto-detects existing Windows cloud sync folders:
    /// `%OneDrive%`, `%OneDriveCommercial%`, `%USERPROFILE%\Google Drive`,
    /// `G:\My Drive`, and `%USERPROFILE%\Dropbox`.
    [[nodiscard]] static std::vector<CloudFolderInfo> detectCloudSyncFolders();

    /// Returns true if `path` resides inside any detected Windows cloud sync folder.
    [[nodiscard]] static bool isPathInCloudSyncFolder(const std::filesystem::path& path);

    /// Computes the deterministic path inside `<exe_dir>/data/offline_cache/` for a comic.
    [[nodiscard]] std::filesystem::path getOfflineCachePath(
        std::int64_t comicId,
        const std::filesystem::path& originalPath,
        mavira::core::ComicFormat format) const;

    /// Returns the best readable path for a comic: prefers the offline-pinned local copy
    /// when pinned and present (especially when the source drive/cloud file is offline).
    [[nodiscard]] std::filesystem::path resolveReadablePath(
        const mavira::data::db::ComicRecord& comic) const;

    /// Copies the selected comic into `<exe_dir>/data/offline_cache/` on a background
    /// worker thread and marks `is_offline_pinned = 1` in SQLite.
    void pinComicOffline(std::int64_t comicId);

    /// Removes the offline-pinned copy from `<exe_dir>/data/offline_cache/` on a background
    /// worker thread and clears `is_offline_pinned = 0` in SQLite.
    void unpinComicOffline(std::int64_t comicId);

signals:
    void scanStarted(int totalFolders);
    void scanProgress(int processedItems, int totalItems, const QString& currentFolder);
    void scanFinished(int indexedCount, int prunedCount, bool cancelled);
    void libraryUpdated();
    void aboutToDeleteComic(std::int64_t comicId, const std::wstring& filePath);
    void comicRemoved(std::int64_t comicId, const std::wstring& filePath);
    void folderOnlineStateChanged(qint64 folderId, const QString& folderPath, bool isOnline);
    void offlinePinProgress(qint64 comicId, qint64 bytesCopied, qint64 totalBytes);
    void offlinePinFinished(qint64 comicId, bool pinned, bool success, const QString& message);
    void errorOccurred(const QString& message);

private:
    struct FolderWatcher;

    struct SingleFolderScanResult {
        std::size_t indexed{0};
        std::size_t pruned{0};
        bool skippedOffline{false};
        bool cancelled{false};
    };

    void enqueueTrackedTask(
        mavira::core::RequestPriority priority,
        std::function<void(std::stop_token)> task,
        std::stop_token extraStopToken = {});

    SingleFolderScanResult scanSingleFolderBlocking(
        const std::filesystem::path& folderPath,
        std::optional<std::int64_t> watchedFolderId,
        const std::string& expectedVolumeGuid,
        std::stop_token stopToken);

    void startWatcherForFolderLocked(const mavira::data::db::WatchedFolderRecord& folder);
    void stopWatcherLocked(std::int64_t folderId);
    void watcherThreadProc(
        std::int64_t folderId,
        std::filesystem::path folderPath,
        std::string volumeGuid,
        void* stopEventHandle);

    void scheduleDebouncedRefresh(std::int64_t folderId, const std::filesystem::path& folderPath);

    void emitQueued(std::function<void()> fn);

    mavira::data::db::DatabaseManager* db_{nullptr};
    mavira::core::cache::CacheManager* cacheManager_{nullptr};
    mavira::data::fs::PortablePaths portablePaths_{};

    std::unique_ptr<mavira::core::threading::WorkerPool> ownedPool_{};
    mavira::core::threading::WorkerPool* pool_{nullptr};

    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> smartRename_{false};
    std::atomic<int> debounceIntervalMs_{kDefaultDebounceMs};
    std::atomic<int> activeScans_{0};
    std::atomic<bool> watching_{false};

    std::stop_source shutdownStopSource_{};

    mutable std::mutex scanStopMutex_{};
    std::shared_ptr<std::stop_source> activeScanStopSource_{};

    mutable std::mutex inflightMutex_{};
    std::condition_variable inflightCv_{};
    std::size_t inflightTasks_{0};

    mutable std::mutex watchersMutex_{};
    std::unordered_map<std::int64_t, std::unique_ptr<FolderWatcher>> watchers_{};

    std::unordered_map<std::int64_t, QTimer*> debounceTimers_{};
    QTimer* orphanPruneTimer_{nullptr};
};

} // namespace mavira::app
