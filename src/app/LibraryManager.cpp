#include "app/LibraryManager.hpp"

#include "core/archive/ArchiveEngine.hpp"
#include "core/archive/PdfDocumentReader.hpp"
#include "core/metadata/MetadataManager.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <QMetaObject>
#include <QMetaType>
#include <QThread>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_set>

namespace mavira::app {

namespace {

constexpr DWORD kOfflinePollIntervalMs = 2000;
constexpr std::size_t kWatchBufferBytes = 64 * 1024;
constexpr std::size_t kCopyChunkBytes = 1024 * 1024; // 1 MB streaming copy chunks
constexpr std::size_t kProgressEmitStep = 25;

[[nodiscard]] std::int64_t currentUnixSeconds() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

[[nodiscard]] std::wstring toLowerWide(std::wstring_view input) {
    std::wstring out;
    out.reserve(input.size());
    for (wchar_t ch : input) {
        out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return out;
}

[[nodiscard]] std::wstring normalizeLowerPathKey(const std::filesystem::path& path) {
    std::wstring raw = mavira::data::fs::FileManager::stripWin32LongPathPrefix(
        path.lexically_normal().native());
    std::replace(raw.begin(), raw.end(), L'/', L'\\');
    while (raw.size() > 3 && raw.back() == L'\\') {
        raw.pop_back();
    }
    return toLowerWide(raw);
}

[[nodiscard]] QString pathToQString(const std::filesystem::path& path) {
    const std::wstring wide = mavira::data::fs::FileManager::stripWin32LongPathPrefix(path.native());
    return QString::fromStdWString(wide);
}

[[nodiscard]] std::optional<std::wstring> readWindowsEnvVar(const wchar_t* name) {
    if (name == nullptr) {
        return std::nullopt;
    }
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string joinTagsAndGenres(
    const std::vector<std::string>& tags,
    const std::vector<std::string>& genres) {
    std::vector<std::string> combined;
    combined.reserve(tags.size() + genres.size());
    std::set<std::string> seenLower;

    auto appendUnique = [&](const std::vector<std::string>& src) {
        for (const auto& item : src) {
            if (item.empty()) {
                continue;
            }
            std::string lower = item;
            std::transform(
                lower.begin(),
                lower.end(),
                lower.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (seenLower.insert(lower).second) {
                combined.push_back(item);
            }
        }
    };

    appendUnique(tags);
    appendUnique(genres);

    std::string joined;
    for (std::size_t i = 0; i < combined.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += combined[i];
    }
    return joined;
}

[[nodiscard]] bool isRelevantWatchNotification(const std::filesystem::path& relativePath) {
    if (relativePath.empty()) {
        return true;
    }

    if (mavira::data::fs::FileManager::detectFormatFromExtension(relativePath) !=
        mavira::core::ComicFormat::Unknown) {
        return true;
    }
    if (mavira::data::fs::FileManager::isSupportedImageExtension(relativePath)) {
        return true;
    }

    const std::wstring filenameLower = toLowerWide(relativePath.filename().native());
    if (filenameLower == L"comicinfo.xml") {
        return true;
    }

    // Directories (which typically have no extension) may have been created, deleted, or renamed.
    if (!relativePath.has_extension()) {
        return true;
    }

    return false;
}

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] mavira::core::Result<void> copySingleFileChunked(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath,
    std::stop_token stopToken,
    const std::function<void(std::uint64_t, std::uint64_t)>& progressCb) {
    const std::wstring srcLong = mavira::data::fs::FileManager::toWin32LongPath(sourcePath);
    const std::filesystem::path tempPath = std::filesystem::path(targetPath.native() + L".tmp");
    const std::wstring tmpLong = mavira::data::fs::FileManager::toWin32LongPath(tempPath);
    const std::wstring dstLong = mavira::data::fs::FileManager::toWin32LongPath(targetPath);

    UniqueHandle srcHandle(::CreateFileW(
        srcLong.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!srcHandle || srcHandle.get() == INVALID_HANDLE_VALUE) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::IoError,
            "Failed to open source file for offline pinning (Win32 error " +
                std::to_string(::GetLastError()) + ")");
    }

    LARGE_INTEGER fileSizeLi{};
    if (!::GetFileSizeEx(srcHandle.get(), &fileSizeLi) || fileSizeLi.QuadPart < 0) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::IoError,
            "Failed to query source file size for offline pinning");
    }
    const std::uint64_t totalBytes = static_cast<std::uint64_t>(fileSizeLi.QuadPart);

    UniqueHandle dstHandle(::CreateFileW(
        tmpLong.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!dstHandle || dstHandle.get() == INVALID_HANDLE_VALUE) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::IoError,
            "Failed to create offline cache temp file (Win32 error " +
                std::to_string(::GetLastError()) + ")");
    }

    auto cleanupTemp = [&]() {
        dstHandle.reset();
        ::DeleteFileW(tmpLong.c_str());
    };

    std::vector<std::uint8_t> buffer(kCopyChunkBytes);
    std::uint64_t copiedBytes = 0;

    if (progressCb) {
        progressCb(0, totalBytes);
    }

    for (;;) {
        if (stopToken.stop_requested()) {
            cleanupTemp();
            return mavira::core::Result<void>::err(
                mavira::core::ErrorCode::Cancelled,
                "Offline pinning cancelled");
        }

        DWORD bytesRead = 0;
        if (!::ReadFile(
                srcHandle.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead,
                nullptr)) {
            const DWORD err = ::GetLastError();
            cleanupTemp();
            return mavira::core::Result<void>::err(
                mavira::core::ErrorCode::IoError,
                "ReadFile failed during offline pinning (Win32 error " + std::to_string(err) + ")");
        }

        if (bytesRead == 0) {
            break;
        }

        DWORD totalWritten = 0;
        while (totalWritten < bytesRead) {
            DWORD writtenNow = 0;
            if (!::WriteFile(
                    dstHandle.get(),
                    buffer.data() + totalWritten,
                    bytesRead - totalWritten,
                    &writtenNow,
                    nullptr)) {
                const DWORD err = ::GetLastError();
                cleanupTemp();
                return mavira::core::Result<void>::err(
                    mavira::core::ErrorCode::IoError,
                    "WriteFile failed during offline pinning (Win32 error " + std::to_string(err) + ")");
            }
            totalWritten += writtenNow;
        }

        copiedBytes += bytesRead;
        if (progressCb) {
            progressCb(copiedBytes, totalBytes);
        }
    }

    ::FlushFileBuffers(dstHandle.get());
    dstHandle.reset();
    srcHandle.reset();

    if (!::MoveFileExW(
            tmpLong.c_str(),
            dstLong.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(tmpLong.c_str());
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::IoError,
            "Failed to finalize offline cache file (Win32 error " + std::to_string(err) + ")");
    }

    return mavira::core::Result<void>::ok();
}

[[nodiscard]] mavira::core::Result<void> copyFolderComicChunked(
    const std::filesystem::path& sourceDir,
    const std::filesystem::path& targetDir,
    std::stop_token stopToken,
    const std::function<void(std::uint64_t, std::uint64_t)>& progressCb) {
    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::path(targetDir.native() + L".tmp");
    std::filesystem::remove_all(tempDir, ec);
    ec.clear();

    std::filesystem::create_directories(tempDir, ec);
    if (ec) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::IoError,
            "Failed to create temporary offline folder: " + ec.message());
    }

    std::vector<std::filesystem::path> filesToCopy;
    std::uint64_t totalBytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             sourceDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) {
            continue;
        }
        const auto& p = entry.path();
        const bool isImg = mavira::data::fs::FileManager::isSupportedImageExtension(p);
        const bool isXml = toLowerWide(p.filename().native()) == L"comicinfo.xml";
        if (!isImg && !isXml) {
            continue;
        }
        filesToCopy.push_back(p);
        const auto sz = entry.file_size(fileEc);
        if (!fileEc) {
            totalBytes += static_cast<std::uint64_t>(sz);
        }
    }

    std::uint64_t copiedTotal = 0;
    if (progressCb) {
        progressCb(0, totalBytes);
    }

    for (const auto& srcFile : filesToCopy) {
        if (stopToken.stop_requested()) {
            std::filesystem::remove_all(tempDir, ec);
            return mavira::core::Result<void>::err(
                mavira::core::ErrorCode::Cancelled,
                "Offline pinning cancelled");
        }

        const std::filesystem::path dstFile = tempDir / srcFile.filename();
        std::uint64_t fileCopiedBase = copiedTotal;
        auto copyRes = copySingleFileChunked(
            srcFile,
            dstFile,
            stopToken,
            [&](std::uint64_t bytesInFile, std::uint64_t) {
                if (progressCb) {
                    progressCb(fileCopiedBase + bytesInFile, totalBytes);
                }
            });
        if (!copyRes) {
            std::filesystem::remove_all(tempDir, ec);
            return copyRes;
        }

        std::error_code sizeEc;
        const auto sz = std::filesystem::file_size(srcFile, sizeEc);
        if (!sizeEc) {
            copiedTotal += static_cast<std::uint64_t>(sz);
        }
    }

    std::filesystem::remove_all(targetDir, ec);
    ec.clear();
    std::filesystem::rename(tempDir, targetDir, ec);
    if (ec) {
        std::filesystem::remove_all(tempDir, ec);
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::IoError,
            "Failed to finalize offline cache folder: " + ec.message());
    }

    return mavira::core::Result<void>::ok();
}

} // namespace

// =============================================================================
// FolderWatcher internal definition
// =============================================================================
struct LibraryManager::FolderWatcher {
    std::int64_t folderId{0};
    std::filesystem::path path{};
    std::string volumeGuid{};
    HANDLE stopEvent{nullptr};
    std::jthread thread{};

    ~FolderWatcher() {
        stop();
    }

    void stop() {
        if (stopEvent != nullptr) {
            ::SetEvent(stopEvent);
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (stopEvent != nullptr) {
            ::CloseHandle(stopEvent);
            stopEvent = nullptr;
        }
    }
};

// =============================================================================
// Construction & Lifecycle
// =============================================================================
LibraryManager::LibraryManager(
    mavira::data::db::DatabaseManager* db,
    mavira::core::cache::CacheManager* cacheManager,
    mavira::data::fs::PortablePaths portablePaths,
    mavira::core::threading::WorkerPool* workerPool,
    QObject* parent)
    : QObject(parent),
      db_(db),
      cacheManager_(cacheManager),
      portablePaths_(std::move(portablePaths)) {
    qRegisterMetaType<std::int64_t>("std::int64_t");
    qRegisterMetaType<std::wstring>("std::wstring");
    if (workerPool != nullptr) {
        pool_ = workerPool;
    } else {
        ownedPool_ = std::make_unique<mavira::core::threading::WorkerPool>();
        pool_ = ownedPool_.get();
    }

    (void)mavira::data::fs::FileManager::ensurePortableDirectories(portablePaths_);
}

LibraryManager::~LibraryManager() {
    shutdown();
}

void LibraryManager::shutdown() {
    const bool alreadyShuttingDown = shuttingDown_.exchange(true);
    if (alreadyShuttingDown) {
        return;
    }

    stopWatching();
    cancelActiveScan();
    shutdownStopSource_.request_stop();

    for (auto& [id, timer] : debounceTimers_) {
        if (timer != nullptr) {
            timer->stop();
        }
    }
    debounceTimers_.clear();
    if (orphanPruneTimer_ != nullptr) {
        orphanPruneTimer_->stop();
    }

    // Wait for every task submitted by this LibraryManager to finish before destruction.
    {
        std::unique_lock<std::mutex> lock(inflightMutex_);
        inflightCv_.wait(lock, [this]() { return inflightTasks_ == 0; });
    }

    if (ownedPool_ != nullptr) {
        ownedPool_->shutdown();
    }
}

void LibraryManager::setSmartRename(bool enabled) noexcept {
    smartRename_.store(enabled, std::memory_order_relaxed);
}

bool LibraryManager::smartRename() const noexcept {
    return smartRename_.load(std::memory_order_relaxed);
}

void LibraryManager::setDebounceIntervalMs(int ms) noexcept {
    debounceIntervalMs_.store(std::max(10, ms), std::memory_order_relaxed);
}

int LibraryManager::debounceIntervalMs() const noexcept {
    return debounceIntervalMs_.load(std::memory_order_relaxed);
}

// =============================================================================
// Task Tracking & Thread-Safe Signal Dispatch
// =============================================================================
void LibraryManager::emitQueued(std::function<void()> fn) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [this, fn = std::move(fn)]() {
            if (!shuttingDown_.load(std::memory_order_acquire)) {
                fn();
            }
        },
        Qt::QueuedConnection);
}

void LibraryManager::enqueueTrackedTask(
    mavira::core::RequestPriority priority,
    std::function<void(std::stop_token)> task,
    std::stop_token extraStopToken) {
    if (shuttingDown_.load(std::memory_order_acquire) || pool_ == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(inflightMutex_);
        ++inflightTasks_;
    }

    auto shutdownToken = shutdownStopSource_.get_token();

    (void)pool_->enqueue(
        priority,
        [this,
         task = std::move(task),
         extraStopToken = std::move(extraStopToken)](std::stop_token poolToken) mutable {
            struct Guard {
                LibraryManager* self;
                ~Guard() {
                    std::lock_guard<std::mutex> lock(self->inflightMutex_);
                    if (self->inflightTasks_ > 0) {
                        --self->inflightTasks_;
                    }
                    if (self->inflightTasks_ == 0) {
                        self->inflightCv_.notify_all();
                    }
                }
            } guard{this};

            if (poolToken.stop_requested() || extraStopToken.stop_requested() ||
                shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }

            // Bridge both poolToken (linked to shutdownToken) and extraStopToken (e.g. scan cancel).
            std::stop_source combinedSource;
            std::stop_callback cb1(poolToken, [&combinedSource]() { combinedSource.request_stop(); });
            std::stop_callback cb2(extraStopToken, [&combinedSource]() { combinedSource.request_stop(); });

            task(combinedSource.get_token());
        },
        shutdownToken);
}

// =============================================================================
// Watched Folders & Background Scanning
// =============================================================================
mavira::core::Result<std::int64_t> LibraryManager::addWatchedFolder(
    const std::filesystem::path& folderPath,
    bool triggerScan) {
    if (db_ == nullptr || !db_->isOpen()) {
        return mavira::core::Result<std::int64_t>::err(
            mavira::core::ErrorCode::DatabaseError, "Database is not open");
    }

    if (folderPath.empty()) {
        return mavira::core::Result<std::int64_t>::err(
            mavira::core::ErrorCode::InvalidArgument, "Watched folder path is empty");
    }

    const std::string volumeGuid = mavira::data::fs::FileManager::getVolumeGuidForPath(folderPath);
    auto addRes = db_->addWatchedFolder(folderPath, volumeGuid);
    if (!addRes) {
        return addRes;
    }

    const std::int64_t folderId = addRes.value();

    if (watching_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(watchersMutex_);
        mavira::data::db::WatchedFolderRecord rec;
        rec.id = folderId;
        rec.path = folderPath;
        rec.volumeGuid = volumeGuid;
        rec.isEnabled = true;
        startWatcherForFolderLocked(rec);
    }

    if (triggerScan) {
        scanFolderAsync(folderPath, folderId);
    }

    return mavira::core::Result<std::int64_t>::ok(folderId);
}

mavira::core::Result<void> LibraryManager::removeWatchedFolder(std::int64_t folderId) {
    if (db_ == nullptr || !db_->isOpen()) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::DatabaseError, "Database is not open");
    }

    {
        std::lock_guard<std::mutex> lock(watchersMutex_);
        stopWatcherLocked(folderId);
    }

    auto res = db_->removeWatchedFolder(folderId);
    if (!res) {
        return res;
    }

    emitQueued([this]() { emit libraryUpdated(); });
    return mavira::core::Result<void>::ok();
}

mavira::core::Result<void> LibraryManager::removeComicFromLibrary(
    std::int64_t comicId,
    bool deleteFromDisk) {
    if (db_ == nullptr || !db_->isOpen()) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::DatabaseError, "Database is not open");
    }
    if (comicId <= 0) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::InvalidArgument, "Invalid comic ID");
    }

    auto comicRes = db_->getComicById(comicId);
    if (!comicRes) {
        return mavira::core::Result<void>::err(comicRes.error());
    }
    if (!comicRes.value().has_value()) {
        return mavira::core::Result<void>::err(
            mavira::core::ErrorCode::NotFound, "Comic record not found in database");
    }

    const auto& comic = *comicRes.value();
    const std::wstring removedFilePath = comic.filePath.native();

    // Release any open reader file handle synchronously before Recycle Bin deletion.
    if (QThread::currentThread() == thread()) {
        emit aboutToDeleteComic(comicId, removedFilePath);
    } else {
        QMetaObject::invokeMethod(
            this,
            [this, comicId, removedFilePath]() {
                emit aboutToDeleteComic(comicId, removedFilePath);
            },
            Qt::BlockingQueuedConnection);
    }

    if (deleteFromDisk && !comic.filePath.empty()) {
        const std::wstring cleanPath =
            mavira::data::fs::FileManager::stripWin32LongPathPrefix(
                comic.filePath.lexically_normal().native());
        std::wstring shellPath = cleanPath;
        std::replace(shellPath.begin(), shellPath.end(), L'/', L'\\');

        std::error_code ec;
        if (std::filesystem::exists(mavira::data::fs::FileManager::toWin32LongPath(comic.filePath), ec) && !ec) {
            // SHFileOperationW requires a double-null-terminated string.
            shellPath.push_back(L'\0');
            shellPath.push_back(L'\0');

            SHFILEOPSTRUCTW fileOp{};
            fileOp.wFunc = FO_DELETE;
            fileOp.pFrom = shellPath.c_str();
            fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

            const int opRc = ::SHFileOperationW(&fileOp);
            if (opRc != 0 || fileOp.fAnyOperationsAborted) {
                return mavira::core::Result<void>::err(
                    mavira::core::ErrorCode::IoError,
                    "Failed to move file to Recycle Bin (SHFileOperationW code " +
                        std::to_string(opRc) + ")");
            }
        }
    }

    // Clean up any offline-pinned cache copy and cover cache entry.
    if (comic.isOfflinePinned) {
        const std::filesystem::path cachedPath =
            getOfflineCachePath(comic.id, comic.filePath, comic.format);
        std::error_code ec;
        if (std::filesystem::exists(cachedPath, ec)) {
            if (comic.format == mavira::core::ComicFormat::Folder) {
                std::filesystem::remove_all(cachedPath, ec);
            } else {
                std::filesystem::remove(cachedPath, ec);
            }
        }
    }
    if (cacheManager_ != nullptr && cacheManager_->coverStorage() != nullptr) {
        cacheManager_->coverStorage()->invalidate(comicId);
    }

    auto delRes = db_->deleteComic(comicId);
    if (!delRes) {
        return delRes;
    }

    if (QThread::currentThread() == thread()) {
        emit comicRemoved(comicId, removedFilePath);
        emit libraryUpdated();
    } else {
        emitQueued([this, comicId, removedFilePath]() {
            emit comicRemoved(comicId, removedFilePath);
            emit libraryUpdated();
        });
    }
    return mavira::core::Result<void>::ok();
}

void LibraryManager::pruneOrphansAsync() {
    if (db_ == nullptr || !db_->isOpen() || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    enqueueTrackedTask(
        mavira::core::RequestPriority::Low,
        [this](std::stop_token stopToken) {
            if (stopToken.stop_requested() || shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            auto pruneRes = db_->pruneAllOrphans(
                [](const std::filesystem::path& p) {
                    return mavira::data::fs::FileManager::isVolumeOnline(p);
                },
                [](const std::filesystem::path& p) {
                    return mavira::data::fs::FileManager::isPathOnline(p);
                });
            if (pruneRes && pruneRes.value().pruned > 0) {
                spdlog::info("Orphan prune removed {} stale comic record(s)", pruneRes.value().pruned);
                emitQueued([this]() { emit libraryUpdated(); });
            }
        });
}

void LibraryManager::cancelActiveScan() {
    std::lock_guard<std::mutex> lock(scanStopMutex_);
    if (activeScanStopSource_ != nullptr) {
        activeScanStopSource_->request_stop();
    }
}

bool LibraryManager::isScanning() const noexcept {
    return activeScans_.load(std::memory_order_acquire) > 0;
}

void LibraryManager::scanAllFoldersAsync() {
    if (db_ == nullptr || !db_->isOpen() || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    std::shared_ptr<std::stop_source> scanStop;
    {
        std::lock_guard<std::mutex> lock(scanStopMutex_);
        scanStop = std::make_shared<std::stop_source>();
        activeScanStopSource_ = scanStop;
    }

    enqueueTrackedTask(
        mavira::core::RequestPriority::Low,
        [this](std::stop_token stopToken) {
            activeScans_.fetch_add(1, std::memory_order_acq_rel);
            struct ScanCounterGuard {
                std::atomic<int>& counter;
                ~ScanCounterGuard() { counter.fetch_sub(1, std::memory_order_acq_rel); }
            } counterGuard{activeScans_};

            auto foldersRes = db_->getWatchedFolders();
            if (!foldersRes) {
                const QString errMsg = QString::fromStdString(foldersRes.error().message);
                emitQueued([this, errMsg]() { emit errorOccurred(errMsg); });
                return;
            }

            std::vector<mavira::data::db::WatchedFolderRecord> enabledFolders;
            for (auto& f : foldersRes.value()) {
                if (f.isEnabled) {
                    enabledFolders.push_back(std::move(f));
                }
            }

            const int totalFolders = static_cast<int>(enabledFolders.size());
            emitQueued([this, totalFolders]() { emit scanStarted(totalFolders); });

            std::size_t totalIndexed = 0;
            std::size_t totalPruned = 0;
            bool wasCancelled = false;

            for (const auto& folder : enabledFolders) {
                if (stopToken.stop_requested()) {
                    wasCancelled = true;
                    break;
                }

                const auto res = scanSingleFolderBlocking(
                    folder.path, folder.id, folder.volumeGuid, stopToken);
                totalIndexed += res.indexed;
                totalPruned += res.pruned;
                if (res.cancelled) {
                    wasCancelled = true;
                    break;
                }
            }

            emitQueued([this, totalIndexed, totalPruned, wasCancelled]() {
                emit libraryUpdated();
                emit scanFinished(
                    static_cast<int>(totalIndexed),
                    static_cast<int>(totalPruned),
                    wasCancelled);
            });
        },
        scanStop->get_token());
}

void LibraryManager::scanFolderAsync(
    const std::filesystem::path& folderPath,
    std::optional<std::int64_t> watchedFolderId) {
    if (db_ == nullptr || !db_->isOpen() || folderPath.empty() ||
        shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    std::shared_ptr<std::stop_source> scanStop;
    {
        std::lock_guard<std::mutex> lock(scanStopMutex_);
        scanStop = std::make_shared<std::stop_source>();
        activeScanStopSource_ = scanStop;
    }

    enqueueTrackedTask(
        mavira::core::RequestPriority::Low,
        [this, folderPath, watchedFolderId](std::stop_token stopToken) {
            activeScans_.fetch_add(1, std::memory_order_acq_rel);
            struct ScanCounterGuard {
                std::atomic<int>& counter;
                ~ScanCounterGuard() { counter.fetch_sub(1, std::memory_order_acq_rel); }
            } counterGuard{activeScans_};

            emitQueued([this]() { emit scanStarted(1); });

            const std::string volGuid =
                mavira::data::fs::FileManager::getVolumeGuidForPath(folderPath);
            const auto res = scanSingleFolderBlocking(
                folderPath, watchedFolderId, volGuid, stopToken);

            emitQueued([this, res]() {
                emit libraryUpdated();
                emit scanFinished(
                    static_cast<int>(res.indexed),
                    static_cast<int>(res.pruned),
                    res.cancelled);
            });
        },
        scanStop->get_token());
}

LibraryManager::SingleFolderScanResult LibraryManager::scanSingleFolderBlocking(
    const std::filesystem::path& folderPath,
    std::optional<std::int64_t> watchedFolderId,
    const std::string& expectedVolumeGuid,
    std::stop_token stopToken) {
    SingleFolderScanResult summary{};
    if (db_ == nullptr || !db_->isOpen()) {
        return summary;
    }

    // Unplugged Drive Guard: if the volume or folder is offline, never scan or prune.
    if (!mavira::data::fs::FileManager::isVolumeOnline(folderPath) ||
        !mavira::data::fs::FileManager::isPathOnline(folderPath)) {
        summary.skippedOffline = true;
        if (watchedFolderId.has_value()) {
            const qint64 fid = *watchedFolderId;
            const QString fpath = pathToQString(folderPath);
            emitQueued([this, fid, fpath]() {
                emit folderOnlineStateChanged(fid, fpath, false);
            });
        }
        return summary;
    }

    auto scanRes = mavira::data::fs::FileManager::scanComicsInFolder(folderPath);
    if (!scanRes) {
        if (scanRes.errorCode() == mavira::core::ErrorCode::OfflineVolume) {
            summary.skippedOffline = true;
            return summary;
        }
        const QString errMsg = QString::fromStdString(scanRes.error().message);
        emitQueued([this, errMsg]() { emit errorOccurred(errMsg); });
        return summary;
    }

    const auto& discovered = scanRes.value();
    const int totalItems = static_cast<int>(discovered.size());
    const QString folderQStr = pathToQString(folderPath);
    const bool useSmartRename = smartRename_.load(std::memory_order_relaxed);
    const std::int64_t nowUnix = currentUnixSeconds();

    emitQueued([this, totalItems, folderQStr]() {
        emit scanProgress(0, totalItems, folderQStr);
    });

    std::unordered_set<std::wstring> existingPathsLower;
    existingPathsLower.reserve(discovered.size());

    std::vector<mavira::data::db::ComicRecord> batch;
    batch.reserve(kCommitBatchSize);

    std::size_t processed = 0;

    auto flushBatch = [&]() -> bool {
        if (batch.empty()) {
            return true;
        }
        auto upsertRes = db_->upsertComicsBatched(batch, kCommitBatchSize);
        if (!upsertRes) {
            const QString errMsg = QString::fromStdString(upsertRes.error().message);
            emitQueued([this, errMsg]() { emit errorOccurred(errMsg); });
            return false;
        }
        summary.indexed += upsertRes.value();
        batch.clear();

        const int procInt = static_cast<int>(processed);
        emitQueued([this, procInt, totalItems, folderQStr]() {
            emit scanProgress(procInt, totalItems, folderQStr);
            emit libraryUpdated();
        });
        return true;
    };

    for (const auto& scanned : discovered) {
        if (stopToken.stop_requested()) {
            summary.cancelled = true;
            break;
        }

        existingPathsLower.insert(normalizeLowerPathKey(scanned.path));

        // Check existing DB row so we preserve reading progress, bookmarks, cover pack offsets,
        // hidden/offline-pinned flags, and skip re-parsing unchanged archives.
        std::optional<mavira::data::db::ComicRecord> existing;
        if (auto existingRes = db_->getComicByPath(scanned.path); existingRes) {
            existing = std::move(existingRes.value());
        }

        const bool unchanged = existing.has_value() &&
                               existing->lastModifiedUnix == scanned.lastModifiedUnix &&
                               existing->fileSizeBytes == scanned.fileSizeBytes &&
                               existing->format == scanned.format &&
                               (existing->pageCount > 0 || scanned.isCloudPlaceholder);

        if (unchanged) {
            ++processed;
            if (processed % kProgressEmitStep == 0 || processed == discovered.size()) {
                const int procInt = static_cast<int>(processed);
                emitQueued([this, procInt, totalItems, folderQStr]() {
                    emit scanProgress(procInt, totalItems, folderQStr);
                });
            }
            continue;
        }

        mavira::data::db::ComicRecord rec;
        if (existing.has_value()) {
            rec = *existing;
            // File changed on disk: invalidate cached cover so it will be re-extracted.
            rec.coverPackOffset = -1;
            rec.coverPackSize = 0;
            if (cacheManager_ != nullptr && cacheManager_->coverStorage() != nullptr) {
                cacheManager_->coverStorage()->invalidate(existing->id);
            }
        } else {
            rec.addedUnix = nowUnix;
        }

        rec.filePath = scanned.path;
        rec.folderPath = scanned.path.parent_path();
        if (watchedFolderId.has_value()) {
            rec.watchedFolderId = watchedFolderId;
        }
        rec.volumeGuid = !scanned.volumeGuid.empty() ? scanned.volumeGuid : expectedVolumeGuid;
        rec.format = scanned.format;
        rec.fileSizeBytes = scanned.fileSizeBytes;
        rec.lastModifiedUnix = scanned.lastModifiedUnix;

        std::optional<std::string> comicInfoXml;
        std::optional<mavira::core::archive::PdfMetadata> pdfMeta;
        std::uint32_t detectedPageCount = 0;

        // Cloud Placeholder Guard: NEVER open cloud-only placeholder files during background
        // scans, otherwise Windows cloud sync providers would trigger mass downloads.
        if (!scanned.isCloudPlaceholder) {
            auto readerRes = mavira::core::archive::ArchiveEngine::openArchive(scanned.path);
            if (readerRes && readerRes.value() != nullptr) {
                auto& reader = readerRes.value();
                detectedPageCount = static_cast<std::uint32_t>(reader->pageCount());

                if (auto xmlRes = reader->readMetadataXml(); xmlRes && xmlRes.value().has_value()) {
                    comicInfoXml = std::move(xmlRes.value());
                }

                if (scanned.format == mavira::core::ComicFormat::PDF) {
                    if (auto* pdfReader =
                            dynamic_cast<mavira::core::archive::PdfDocumentReader*>(reader.get())) {
                        pdfMeta = pdfReader->pdfMetadata();
                    }
                }
            }
        }

        const auto meta = mavira::core::metadata::MetadataManager::extractMetadata(
            scanned.path, comicInfoXml, pdfMeta, useSmartRename);

        rec.title = !meta.displayTitle.empty() ? meta.displayTitle : meta.title;
        rec.series = meta.series;
        rec.author = meta.writer;
        rec.tags = joinTagsAndGenres(meta.tags, meta.genres);
        if (detectedPageCount > 0) {
            rec.pageCount = detectedPageCount;
        } else if (meta.pageCount.has_value() && *meta.pageCount > 0) {
            rec.pageCount = static_cast<std::uint32_t>(*meta.pageCount);
        }

        batch.push_back(std::move(rec));
        ++processed;

        if (batch.size() >= kCommitBatchSize) {
            if (!flushBatch()) {
                return summary;
            }
        } else if (processed % kProgressEmitStep == 0) {
            const int procInt = static_cast<int>(processed);
            emitQueued([this, procInt, totalItems, folderQStr]() {
                emit scanProgress(procInt, totalItems, folderQStr);
            });
        }
    }

    if (!flushBatch()) {
        return summary;
    }

    // Safe Orphan Pruning (Unplugged Drive Guard):
    // Only prune missing records if scan completed without cancellation AND the volume/folder
    // is still online. DatabaseManager::pruneOrphans also protects is_offline_pinned == 1 rows.
    if (!summary.cancelled &&
        mavira::data::fs::FileManager::isVolumeOnline(folderPath) &&
        mavira::data::fs::FileManager::isPathOnline(folderPath)) {
        auto pruneRes = db_->pruneOrphans(
            folderPath,
            existingPathsLower,
            [](const std::filesystem::path& p) {
                return mavira::data::fs::FileManager::isVolumeOnline(p) &&
                       mavira::data::fs::FileManager::isPathOnline(p);
            });
        if (pruneRes) {
            summary.pruned = pruneRes.value().pruned;
        }
    }

    if (!summary.cancelled && watchedFolderId.has_value()) {
        (void)db_->updateWatchedFolderScanTime(*watchedFolderId, currentUnixSeconds());
    }

    const int finalProcessed = static_cast<int>(processed);
    emitQueued([this, finalProcessed, totalItems, folderQStr]() {
        emit scanProgress(finalProcessed, totalItems, folderQStr);
    });

    return summary;
}

void LibraryManager::indexCoversAsync() {
    if (cacheManager_ == nullptr || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    auto* packProvider =
        dynamic_cast<mavira::core::cache::PackFileCoverProvider*>(cacheManager_->coverStorage());
    if (packProvider == nullptr) {
        return;
    }

    enqueueTrackedTask(
        mavira::core::RequestPriority::Low,
        [this, packProvider](std::stop_token stopToken) {
            auto res = packProvider->indexUncachedCovers(
                stopToken,
                [this](std::size_t, std::size_t) {
                    emitQueued([this]() { emit libraryUpdated(); });
                });
            if (res && res.value() > 0) {
                emitQueued([this]() { emit libraryUpdated(); });
            }
        });
}

// =============================================================================
// Win32 ReadDirectoryChangesW Live Folder Watchers
// =============================================================================
void LibraryManager::startWatching() {
    if (db_ == nullptr || !db_->isOpen() || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    auto foldersRes = db_->getWatchedFolders();
    if (!foldersRes) {
        const QString errMsg = QString::fromStdString(foldersRes.error().message);
        emitQueued([this, errMsg]() { emit errorOccurred(errMsg); });
        return;
    }

    std::lock_guard<std::mutex> lock(watchersMutex_);
    watching_.store(true, std::memory_order_release);

    for (const auto& folder : foldersRes.value()) {
        if (!folder.isEnabled) {
            continue;
        }
        startWatcherForFolderLocked(folder);
    }

    // Run an async startup orphan prune pass and schedule periodic checks.
    pruneOrphansAsync();
    if (QThread::currentThread() == thread()) {
        if (orphanPruneTimer_ == nullptr) {
            orphanPruneTimer_ = new QTimer(this);
            orphanPruneTimer_->setInterval(kDefaultOrphanPruneIntervalMs);
            QObject::connect(orphanPruneTimer_, &QTimer::timeout, this, &LibraryManager::pruneOrphansAsync);
        }
        orphanPruneTimer_->start();
    }
}

void LibraryManager::stopWatching() {
    if (orphanPruneTimer_ != nullptr && QThread::currentThread() == thread()) {
        orphanPruneTimer_->stop();
    }
    std::unordered_map<std::int64_t, std::unique_ptr<FolderWatcher>> toStop;
    {
        std::lock_guard<std::mutex> lock(watchersMutex_);
        watching_.store(false, std::memory_order_release);
        toStop.swap(watchers_);
    }

    // Signal all stop events first so watcher threads wake up concurrently.
    for (auto& [id, watcher] : toStop) {
        if (watcher != nullptr && watcher->stopEvent != nullptr) {
            ::SetEvent(watcher->stopEvent);
        }
    }
    toStop.clear();
}

bool LibraryManager::isWatching() const noexcept {
    return watching_.load(std::memory_order_acquire);
}

std::size_t LibraryManager::activeWatcherCount() const {
    std::lock_guard<std::mutex> lock(watchersMutex_);
    return watchers_.size();
}

void LibraryManager::startWatcherForFolderLocked(
    const mavira::data::db::WatchedFolderRecord& folder) {
    if (watchers_.find(folder.id) != watchers_.end()) {
        return;
    }

    HANDLE stopEvt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvt == nullptr) {
        return;
    }

    auto watcher = std::make_unique<FolderWatcher>();
    watcher->folderId = folder.id;
    watcher->path = folder.path;
    watcher->volumeGuid = folder.volumeGuid;
    watcher->stopEvent = stopEvt;

    watcher->thread = std::jthread([this,
                                    folderId = folder.id,
                                    folderPath = folder.path,
                                    volumeGuid = folder.volumeGuid,
                                    stopEvt]() {
        watcherThreadProc(folderId, folderPath, volumeGuid, stopEvt);
    });

    watchers_.emplace(folder.id, std::move(watcher));
}

void LibraryManager::stopWatcherLocked(std::int64_t folderId) {
    auto it = watchers_.find(folderId);
    if (it != watchers_.end()) {
        watchers_.erase(it); // ~FolderWatcher signals stopEvent and joins thread.
    }
}

void LibraryManager::watcherThreadProc(
    std::int64_t folderId,
    std::filesystem::path folderPath,
    std::string /*volumeGuid*/,
    void* stopEventHandle) {
    const HANDLE stopEvent = static_cast<HANDLE>(stopEventHandle);
    UniqueHandle ioEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ioEvent) {
        return;
    }

    std::vector<std::uint8_t> buffer(kWatchBufferBytes);
    bool lastReportedOnline = true;
    bool everReportedState = false;

    const QString folderQStr = pathToQString(folderPath);

    auto reportOnlineState = [&](bool isOnline) {
        if (!everReportedState || lastReportedOnline != isOnline) {
            everReportedState = true;
            lastReportedOnline = isOnline;
            emitQueued([this, folderId, folderQStr, isOnline]() {
                emit folderOnlineStateChanged(folderId, folderQStr, isOnline);
            });
        }
    };

    for (;;) {
        if (::WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 ||
            shuttingDown_.load(std::memory_order_acquire)) {
            break;
        }

        // Unplugged Drive Guard: if the drive or folder is currently offline, wait
        // without touching or pruning database entries until it comes back online.
        const bool onlineNow =
            mavira::data::fs::FileManager::isVolumeOnline(folderPath) &&
            mavira::data::fs::FileManager::isPathOnline(folderPath);
        if (!onlineNow) {
            reportOnlineState(false);
            const DWORD waitRes = ::WaitForSingleObject(stopEvent, kOfflinePollIntervalMs);
            if (waitRes == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        const bool recoveredFromOffline = everReportedState && !lastReportedOnline;
        reportOnlineState(true);
        if (recoveredFromOffline) {
            // Drive/folder reconnected: schedule incremental refresh.
            emitQueued([this, folderId, folderPath]() {
                scheduleDebouncedRefresh(folderId, folderPath);
            });
        }

        const std::wstring longPath = mavira::data::fs::FileManager::toWin32LongPath(folderPath);
        UniqueHandle dirHandle(::CreateFileW(
            longPath.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr));

        if (!dirHandle || dirHandle.get() == INVALID_HANDLE_VALUE) {
            reportOnlineState(false);
            const DWORD waitRes = ::WaitForSingleObject(stopEvent, kOfflinePollIntervalMs);
            if (waitRes == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        constexpr DWORD kNotifyFilter =
            FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE;

        for (;;) {
            if (::WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 ||
                shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }

            ::ResetEvent(ioEvent.get());
            OVERLAPPED overlapped{};
            overlapped.hEvent = ioEvent.get();

            const BOOL issued = ::ReadDirectoryChangesW(
                dirHandle.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                TRUE, // Watch nested subdirectories recursively
                kNotifyFilter,
                nullptr,
                &overlapped,
                nullptr);

            if (!issued) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_IO_PENDING) {
                    // Handle invalidated (e.g., external drive unplugged).
                    break;
                }
            }

            HANDLE waitHandles[2] = {stopEvent, ioEvent.get()};
            const DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                // Stop requested: cancel pending overlapped I/O before leaving scope so
                // the kernel cannot write into our stack OVERLAPPED or buffer.
                ::CancelIoEx(dirHandle.get(), &overlapped);
                DWORD unusedBytes = 0;
                (void)::GetOverlappedResult(dirHandle.get(), &overlapped, &unusedBytes, TRUE);
                return;
            }

            if (waitResult != WAIT_OBJECT_0 + 1) {
                ::CancelIoEx(dirHandle.get(), &overlapped);
                DWORD unusedBytes = 0;
                (void)::GetOverlappedResult(dirHandle.get(), &overlapped, &unusedBytes, TRUE);
                break;
            }

            DWORD bytesTransferred = 0;
            if (!::GetOverlappedResult(dirHandle.get(), &overlapped, &bytesTransferred, FALSE)) {
                const DWORD err = ::GetLastError();
                if (err == ERROR_NOTIFY_ENUM_DIR) {
                    // Buffer overflowed due to burst activity: schedule full incremental refresh
                    // only if the folder is still online (Unplugged Drive Guard).
                    if (mavira::data::fs::FileManager::isVolumeOnline(folderPath) &&
                        mavira::data::fs::FileManager::isPathOnline(folderPath)) {
                        emitQueued([this, folderId, folderPath]() {
                            scheduleDebouncedRefresh(folderId, folderPath);
                        });
                    }
                    continue;
                }
                // Device removed, access denied, or network share dropped.
                break;
            }

            bool relevantChange = (bytesTransferred == 0);
            std::size_t offset = 0;
            while (offset < bytesTransferred) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                    buffer.data() + offset);
                const std::size_t charCount = info->FileNameLength / sizeof(wchar_t);
                const std::wstring relName(info->FileName, charCount);
                if (isRelevantWatchNotification(std::filesystem::path(relName))) {
                    relevantChange = true;
                    break;
                }
                if (info->NextEntryOffset == 0) {
                    break;
                }
                offset += info->NextEntryOffset;
            }

            if (relevantChange) {
                // Enforce Unplugged Drive Guard before scheduling refresh.
                if (mavira::data::fs::FileManager::isVolumeOnline(folderPath) &&
                    mavira::data::fs::FileManager::isPathOnline(folderPath)) {
                    emitQueued([this, folderId, folderPath]() {
                        scheduleDebouncedRefresh(folderId, folderPath);
                    });
                } else {
                    reportOnlineState(false);
                    break;
                }
            }
        }
    }
}

void LibraryManager::scheduleDebouncedRefresh(
    std::int64_t folderId,
    const std::filesystem::path& folderPath) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    QTimer* timer = nullptr;
    auto it = debounceTimers_.find(folderId);
    if (it == debounceTimers_.end() || it->second == nullptr) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, this, [this, folderId, folderPath]() {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            // Final Unplugged Drive Guard check before dispatching background scan.
            if (!mavira::data::fs::FileManager::isVolumeOnline(folderPath) ||
                !mavira::data::fs::FileManager::isPathOnline(folderPath)) {
                emit folderOnlineStateChanged(folderId, pathToQString(folderPath), false);
                return;
            }
            scanFolderAsync(folderPath, folderId);
        });
        debounceTimers_[folderId] = timer;
    } else {
        timer = it->second;
    }

    timer->start(debounceIntervalMs_.load(std::memory_order_relaxed));
}

// =============================================================================
// Windows Cloud Sync Detection & Background Offline Pinning
// =============================================================================
std::vector<CloudFolderInfo> LibraryManager::detectCloudSyncFolders() {
    std::vector<CloudFolderInfo> candidates;

    if (auto oneDrive = readWindowsEnvVar(L"OneDrive")) {
        candidates.push_back(CloudFolderInfo{"OneDrive", std::filesystem::path(*oneDrive)});
    }
    if (auto oneDriveCommercial = readWindowsEnvVar(L"OneDriveCommercial")) {
        candidates.push_back(
            CloudFolderInfo{"OneDrive Commercial", std::filesystem::path(*oneDriveCommercial)});
    }
    if (auto userProfile = readWindowsEnvVar(L"USERPROFILE")) {
        const std::filesystem::path profileDir(*userProfile);
        candidates.push_back(CloudFolderInfo{"Google Drive", profileDir / L"Google Drive"});
        candidates.push_back(CloudFolderInfo{"Dropbox", profileDir / L"Dropbox"});
    }
    if ((::GetLogicalDrives() & (1u << (L'G' - L'A'))) != 0) {
        candidates.push_back(
            CloudFolderInfo{"Google Drive", std::filesystem::path(L"G:\\My Drive")});
    }

    std::vector<CloudFolderInfo> detected;
    std::unordered_set<std::wstring> seenNormalized;

    for (auto& candidate : candidates) {
        if (candidate.path.empty()) {
            continue;
        }
        if (!mavira::data::fs::FileManager::isPathOnline(candidate.path)) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate.path, ec) || ec) {
            continue;
        }

        const std::wstring key = normalizeLowerPathKey(candidate.path);
        if (seenNormalized.insert(key).second) {
            candidate.path = candidate.path.lexically_normal();
            detected.push_back(std::move(candidate));
        }
    }

    return detected;
}

bool LibraryManager::isPathInCloudSyncFolder(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    const std::wstring targetKey = normalizeLowerPathKey(path);
    const auto cloudFolders = detectCloudSyncFolders();
    for (const auto& cloud : cloudFolders) {
        const std::wstring rootKey = normalizeLowerPathKey(cloud.path);
        if (targetKey == rootKey) {
            return true;
        }
        if (targetKey.size() > rootKey.size() &&
            targetKey.starts_with(rootKey) &&
            targetKey[rootKey.size()] == L'\\') {
            return true;
        }
    }
    return false;
}

std::filesystem::path LibraryManager::getOfflineCachePath(
    std::int64_t comicId,
    const std::filesystem::path& originalPath,
    mavira::core::ComicFormat format) const {
    const std::wstring baseName = L"comic_" + std::to_wstring(comicId);
    if (format == mavira::core::ComicFormat::Folder) {
        return portablePaths_.offlineCacheDir / baseName;
    }
    const std::wstring ext = originalPath.extension().native();
    return portablePaths_.offlineCacheDir / (baseName + ext);
}

std::filesystem::path LibraryManager::resolveReadablePath(
    const mavira::data::db::ComicRecord& comic) const {
    if (comic.isOfflinePinned) {
        const std::filesystem::path cached =
            getOfflineCachePath(comic.id, comic.filePath, comic.format);
        if (mavira::data::fs::FileManager::isPathOnline(cached)) {
            return cached;
        }
    }
    return comic.filePath;
}

void LibraryManager::pinComicOffline(std::int64_t comicId) {
    if (db_ == nullptr || !db_->isOpen() || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    enqueueTrackedTask(
        mavira::core::RequestPriority::Normal,
        [this, comicId](std::stop_token stopToken) {
            auto comicRes = db_->getComicById(comicId);
            if (!comicRes || !comicRes.value().has_value()) {
                const QString msg = QStringLiteral("Comic not found in database");
                emitQueued([this, comicId, msg]() {
                    emit offlinePinFinished(comicId, false, false, msg);
                });
                return;
            }

            const auto& comic = *comicRes.value();
            if (!mavira::data::fs::FileManager::isPathOnline(comic.filePath)) {
                const QString msg = QStringLiteral("Source comic is offline or unreachable");
                emitQueued([this, comicId, msg]() {
                    emit offlinePinFinished(comicId, false, false, msg);
                });
                return;
            }

            auto dirRes = mavira::data::fs::FileManager::ensurePortableDirectories(portablePaths_);
            if (!dirRes) {
                const QString msg = QString::fromStdString(dirRes.error().message);
                emitQueued([this, comicId, msg]() {
                    emit offlinePinFinished(comicId, false, false, msg);
                });
                return;
            }

            const std::filesystem::path targetPath =
                getOfflineCachePath(comic.id, comic.filePath, comic.format);

            auto progressCb = [this, comicId](std::uint64_t copied, std::uint64_t total) {
                emitQueued([this, comicId, copied, total]() {
                    emit offlinePinProgress(
                        comicId,
                        static_cast<qint64>(copied),
                        static_cast<qint64>(total));
                });
            };

            const auto copyRes = (comic.format == mavira::core::ComicFormat::Folder)
                ? copyFolderComicChunked(comic.filePath, targetPath, stopToken, progressCb)
                : copySingleFileChunked(comic.filePath, targetPath, stopToken, progressCb);

            if (!copyRes) {
                const QString msg = QString::fromStdString(copyRes.error().message);
                emitQueued([this, comicId, msg]() {
                    emit offlinePinFinished(comicId, false, false, msg);
                });
                return;
            }

            auto dbRes = db_->setComicOfflinePinned(comicId, true);
            if (!dbRes) {
                const QString msg = QString::fromStdString(dbRes.error().message);
                emitQueued([this, comicId, msg]() {
                    emit offlinePinFinished(comicId, false, false, msg);
                });
                return;
            }

            emitQueued([this, comicId]() {
                emit libraryUpdated();
                emit offlinePinFinished(comicId, true, true, QString());
            });
        });
}

void LibraryManager::unpinComicOffline(std::int64_t comicId) {
    if (db_ == nullptr || !db_->isOpen() || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    enqueueTrackedTask(
        mavira::core::RequestPriority::Normal,
        [this, comicId](std::stop_token) {
            auto comicRes = db_->getComicById(comicId);
            if (!comicRes || !comicRes.value().has_value()) {
                const QString msg = QStringLiteral("Comic not found in database");
                emitQueued([this, comicId, msg]() {
                    emit offlinePinFinished(comicId, false, false, msg);
                });
                return;
            }

            const auto& comic = *comicRes.value();
            const std::filesystem::path cachedPath =
                getOfflineCachePath(comic.id, comic.filePath, comic.format);

            std::error_code ec;
            if (std::filesystem::exists(cachedPath, ec)) {
                if (comic.format == mavira::core::ComicFormat::Folder) {
                    std::filesystem::remove_all(cachedPath, ec);
                } else {
                    std::filesystem::remove(cachedPath, ec);
                }
            }

            auto dbRes = db_->setComicOfflinePinned(comicId, false);
            if (!dbRes) {
                const QString msg = QString::fromStdString(dbRes.error().message);
                emitQueued([this, comicId, msg]() {
                    emit offlinePinFinished(comicId, false, false, msg);
                });
                return;
            }

            emitQueued([this, comicId]() {
                emit libraryUpdated();
                emit offlinePinFinished(comicId, false, true, QString());
            });
        });
}

} // namespace mavira::app
