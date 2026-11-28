#include "app/ReaderController.hpp"

#include "core/archive/ArchiveEngine.hpp"
#include "core/archive/LibArchiveReader.hpp"
#include "core/archive/PdfDocumentReader.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "data/fs/FileManager.hpp"

#include <QMetaObject>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <unordered_set>
#include <utility>

namespace mavira::app {

namespace {

using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::RequestPriority;
using mavira::core::Result;
using mavira::core::archive::ArchiveEngine;
using mavira::core::archive::IArchiveReader;
using mavira::core::archive::PdfDocumentReader;
using mavira::core::cache::CacheManager;
using mavira::core::cache::PageCacheKey;
using mavira::core::imaging::ImageDecoder;
using mavira::core::rendering::FitMode;
using mavira::core::rendering::LayoutOptions;
using mavira::core::rendering::ReadingDirection;
using mavira::core::rendering::ReadingMode;
using mavira::core::rendering::RenderingEngine;
using mavira::core::rendering::SpreadLayoutResult;
using mavira::core::threading::TaskHandle;
using mavira::core::threading::WorkerPool;
using mavira::data::config::AppSettings;
using mavira::data::db::BookmarkRecord;
using mavira::data::db::ComicRecord;
using mavira::data::db::DatabaseManager;

[[nodiscard]] std::int64_t currentUnixSeconds() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::atomic<std::int64_t> gNextSyntheticComicId{-1000};

[[nodiscard]] std::wstring normalizeLowerPathKey(const std::filesystem::path& path) {
    std::wstring raw = mavira::data::fs::FileManager::stripWin32LongPathPrefix(
        path.lexically_normal().native());
    std::replace(raw.begin(), raw.end(), L'/', L'\\');
    while (raw.size() > 3 && raw.back() == L'\\') {
        raw.pop_back();
    }
    for (wchar_t& ch : raw) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return raw;
}

} // namespace

ReaderController::ReaderController(
    DatabaseManager* db,
    CacheManager* cacheManager,
    WorkerPool* workerPool,
    QObject* parent)
    : QObject(parent)
    , db_(db) {
    qRegisterMetaType<SpreadLayoutResult>("mavira::core::rendering::SpreadLayoutResult");

    if (cacheManager != nullptr) {
        cacheManager_ = cacheManager;
    } else {
        ownedCache_ = std::make_unique<CacheManager>();
        cacheManager_ = ownedCache_.get();
    }

    if (workerPool != nullptr) {
        pool_ = workerPool;
    } else {
        ownedPool_ = std::make_unique<WorkerPool>();
        pool_ = ownedPool_.get();
    }

    progressDebounceTimer_ = new QTimer(this);
    progressDebounceTimer_->setSingleShot(true);
    progressDebounceTimer_->setInterval(progressDebounceMs_);
    connect(progressDebounceTimer_, &QTimer::timeout, this, [this]() {
        flushReadingProgress();
    });
}

ReaderController::~ReaderController() {
    shutdown();
}

void ReaderController::shutdown() {
    const bool wasShuttingDown = shuttingDown_.exchange(true, std::memory_order_acq_rel);
    if (wasShuttingDown) {
        return;
    }

    alive_->store(false, std::memory_order_release);

    if (progressDebounceTimer_ != nullptr) {
        progressDebounceTimer_->stop();
    }
    flushReadingProgress();
    cancelAllInFlightTasks();

    {
        std::unique_lock lock(workerWaitMutex_);
        workerWaitCv_.wait(lock, [this]() { return runningWorkers_ == 0; });
    }

    session_.reset();
    if (ownedPool_ != nullptr) {
        ownedPool_->shutdown();
    }
}

Result<void> ReaderController::openComic(
    const std::filesystem::path& path,
    std::int64_t comicId,
    std::size_t startPage) {
    auto readerRes = ArchiveEngine::openArchive(path);
    if (!readerRes.isOk()) {
        emit errorOccurred(QString::fromStdString(readerRes.error().message));
        return Result<void>::err(readerRes.error());
    }

    return openReader(std::move(readerRes.value()), comicId, startPage, path);
}

Result<void> ReaderController::openComicRecord(const ComicRecord& comic) {
    applyPerComicProfile(comic.readingMode, comic.readingDir, comic.fitMode);
    return openComic(comic.filePath, comic.id, static_cast<std::size_t>(comic.currentPage));
}

Result<void> ReaderController::openReader(
    std::unique_ptr<IArchiveReader> reader,
    std::int64_t comicId,
    std::size_t startPage,
    const std::filesystem::path& sourcePath) {
    if (reader == nullptr || !reader->isOpen()) {
        return Result<void>::err(
            ErrorCode::InvalidArgument,
            "Archive reader must be non-null and already open");
    }

    const std::size_t count = reader->pageCount();
    if (count == 0) {
        return Result<void>::err(ErrorCode::NotFound, "Comic archive contains zero pages");
    }

    if (isOpen()) {
        closeComic();
    }

    ++sessionGeneration_;
    auto newSession = std::make_shared<ReaderSession>();
    newSession->isPdf = (dynamic_cast<PdfDocumentReader*>(reader.get()) != nullptr);
    newSession->isSequential =
        (dynamic_cast<mavira::core::archive::LibArchiveReader*>(reader.get()) != nullptr);
    newSession->reader = std::move(reader);

    session_ = std::move(newSession);
    currentPath_ = sourcePath;
    comicId_ = comicId;
    syntheticCacheId_ = (comicId_ > 0)
        ? comicId_
        : gNextSyntheticComicId.fetch_sub(1, std::memory_order_relaxed);
    totalPages_ = count;
    currentPage_ = (std::min)(startPage, totalPages_ - 1);
    lastPagesConsumed_ = 1;
    currentLayout_.reset();
    progressDirty_ = false;

    cacheManager_->pageCache().setCurrentReadingPosition(
        effectiveCacheComicId(), currentPage_, session_ != nullptr && session_->isSequential);
    loadBookmarks();

    if (comicId > 0 && db_ != nullptr && db_->isOpen() && !hasPerComicProfile_ && !pendingAutoWebtoonDetect_) {
        if (auto recRes = db_->getComicById(comicId); recRes && recRes.value().has_value()) {
            const auto& rec = *recRes.value();
            applyPerComicProfile(rec.readingMode, rec.readingDir, rec.fitMode);
        }
    }

    emit comicOpened(comicId_, static_cast<int>(totalPages_));
    emit pageChanged(static_cast<int>(currentPage_), static_cast<int>(totalPages_));

    requestCurrentAndPrefetchPages();
    return Result<void>::ok();
}

void ReaderController::closeComic() {
    if (!isOpen()) {
        return;
    }

    if (progressDebounceTimer_ != nullptr) {
        progressDebounceTimer_->stop();
    }
    flushReadingProgress();
    cancelAllInFlightTasks();

    ++sessionGeneration_;
    session_.reset();
    currentPath_.clear();
    comicId_ = 0;
    syntheticCacheId_ = 0;
    totalPages_ = 0;
    currentPage_ = 0;
    lastPagesConsumed_ = 1;
    currentLayout_.reset();
    bookmarks_.clear();
    progressDirty_ = false;
    hasPerComicProfile_ = false;
    pendingAutoWebtoonDetect_ = false;

    emit bookmarksChanged();
    emit comicClosed();
}

bool ReaderController::closeIfMatching(std::int64_t comicId, const std::wstring& filePath) {
    if (!isOpen()) {
        return false;
    }

    const bool idMatches = (comicId > 0 && comicId_ == comicId);
    bool pathMatches = false;
    if (!idMatches && !filePath.empty() && !currentPath_.empty()) {
        const std::wstring curKey = normalizeLowerPathKey(currentPath_);
        const std::wstring targetKey = normalizeLowerPathKey(std::filesystem::path(filePath));
        pathMatches = !curKey.empty() && (curKey == targetKey);
    }

    if (!idMatches && !pathMatches) {
        return false;
    }

    if (progressDebounceTimer_ != nullptr) {
        progressDebounceTimer_->stop();
    }
    progressDirty_ = false;
    cancelAllInFlightTasks();

    const std::int64_t cacheKeyId = effectiveCacheComicId();
    ++sessionGeneration_;
    if (session_ != nullptr) {
        std::scoped_lock ioLock(session_->ioMutex);
        if (session_->reader != nullptr) {
            session_->reader->close();
            session_->reader.reset();
        }
    }
    session_.reset();

    if (cacheManager_ != nullptr) {
        cacheManager_->pageCache().eraseComic(cacheKeyId);
        if (comicId > 0 && comicId != cacheKeyId) {
            cacheManager_->pageCache().eraseComic(comicId);
        }
    }

    currentPath_.clear();
    comicId_ = 0;
    syntheticCacheId_ = 0;
    totalPages_ = 0;
    currentPage_ = 0;
    lastPagesConsumed_ = 1;
    currentLayout_.reset();
    bookmarks_.clear();

    emit bookmarksChanged();
    emit comicClosed();
    emit activeComicClosed();
    return true;
}

bool ReaderController::isOpen() const noexcept {
    return session_ != nullptr && session_->reader != nullptr && totalPages_ > 0;
}

bool ReaderController::goToPage(std::size_t pageIndex) {
    if (!isOpen()) {
        return false;
    }

    const std::size_t clamped = (std::min)(pageIndex, totalPages_ - 1);
    const bool changed = (clamped != currentPage_);
    currentPage_ = clamped;
    cacheManager_->pageCache().setCurrentReadingPosition(
        effectiveCacheComicId(), currentPage_, session_ != nullptr && session_->isSequential);

    if (changed) {
        emit pageChanged(static_cast<int>(currentPage_), static_cast<int>(totalPages_));
        scheduleProgressSave();
    }

    requestCurrentAndPrefetchPages();
    return true;
}

bool ReaderController::nextPage() {
    if (!isOpen()) {
        return false;
    }

    const std::size_t step = (std::max)(std::size_t{1}, lastPagesConsumed_);
    if (currentPage_ + step >= totalPages_) {
        if (currentPage_ + 1 >= totalPages_) {
            return false;
        }
        return goToPage(totalPages_ - 1);
    }
    return goToPage(currentPage_ + step);
}

bool ReaderController::previousPage() {
    if (!isOpen() || currentPage_ == 0) {
        return false;
    }

    std::size_t step = 1;
    if (readingMode_ == ReadingMode::DualPage && currentPage_ >= 2) {
        // Check if the immediately preceding page is cached as a wide spread.
        const auto prevFrame = cacheManager_->pageCache().get(
            PageCacheKey{effectiveCacheComicId(), currentPage_ - 1});
        if (!smartWideSpread_ || prevFrame == nullptr || !RenderingEngine::isWideSpread(*prevFrame)) {
            step = 2;
        }
    }

    const std::size_t target = (currentPage_ > step) ? (currentPage_ - step) : 0;
    return goToPage(target);
}

bool ReaderController::firstPage() {
    return goToPage(0);
}

bool ReaderController::lastPage() {
    if (!isOpen()) {
        return false;
    }
    return goToPage(totalPages_ - 1);
}

void ReaderController::applySettings(const AppSettings& settings) {
    applyingProfileOrSettings_ = true;

    globalDefaultDirection_ = settings.default_manga_mode ? ReadingDirection::RTL : ReadingDirection::LTR;
    smartWideSpread_ = settings.smart_wide_spread;
    autoCropBorders_ = settings.auto_crop_borders;

    if (settings.vertical_webtoon_mode || settings.default_page_layout == "webtoon") {
        globalDefaultMode_ = ReadingMode::VerticalWebtoon;
    } else if (settings.default_page_layout == "dual" || settings.default_page_layout == "dual_spread") {
        globalDefaultMode_ = ReadingMode::DualPage;
    } else {
        globalDefaultMode_ = ReadingMode::SinglePage;
    }

    if (settings.default_fit_mode == "fit_width") {
        globalDefaultFitMode_ = FitMode::FitWidth;
    } else if (settings.default_fit_mode == "fit_height") {
        globalDefaultFitMode_ = FitMode::FitHeight;
    } else if (settings.default_fit_mode == "original") {
        globalDefaultFitMode_ = FitMode::Original;
    } else {
        globalDefaultFitMode_ = FitMode::FitScreen;
    }

    if (!hasPerComicProfile_) {
        readingMode_ = globalDefaultMode_;
        readingDirection_ = globalDefaultDirection_;
        fitMode_ = globalDefaultFitMode_;
    }

    applyingProfileOrSettings_ = false;
    emit viewStateChanged();
    if (isOpen()) {
        requestCurrentAndPrefetchPages();
    }
}

void ReaderController::applyPerComicProfile(int readingMode, int readingDir, int fitMode) {
    applyingProfileOrSettings_ = true;
    if (readingMode >= 0 || readingDir >= 0 || fitMode >= 0) {
        hasPerComicProfile_ = true;
        pendingAutoWebtoonDetect_ = false;
        readingMode_ = (readingMode >= 0 && readingMode <= 2)
            ? static_cast<ReadingMode>(readingMode)
            : globalDefaultMode_;
        readingDirection_ = (readingDir >= 0 && readingDir <= 1)
            ? static_cast<ReadingDirection>(readingDir)
            : globalDefaultDirection_;
        fitMode_ = (fitMode >= 0 && fitMode <= 3)
            ? static_cast<FitMode>(fitMode)
            : globalDefaultFitMode_;
    } else {
        hasPerComicProfile_ = false;
        pendingAutoWebtoonDetect_ = true;
        readingMode_ = globalDefaultMode_;
        readingDirection_ = globalDefaultDirection_;
        fitMode_ = globalDefaultFitMode_;
    }
    applyingProfileOrSettings_ = false;
    emit viewStateChanged();
}

void ReaderController::persistComicReadingProfile() {
    if (applyingProfileOrSettings_ || !isOpen() || comicId_ <= 0 || db_ == nullptr || !db_->isOpen()) {
        return;
    }
    hasPerComicProfile_ = true;
    pendingAutoWebtoonDetect_ = false;
    (void)db_->updateComicReadingProfile(
        comicId_,
        static_cast<int>(readingMode_),
        static_cast<int>(readingDirection_),
        static_cast<int>(fitMode_));
}

void ReaderController::setReadingMode(ReadingMode mode) {
    if (readingMode_ == mode) {
        return;
    }
    readingMode_ = mode;
    persistComicReadingProfile();
    emit viewStateChanged();
    if (isOpen()) {
        requestCurrentAndPrefetchPages();
    }
}

void ReaderController::setReadingDirection(ReadingDirection direction) {
    if (readingDirection_ == direction) {
        return;
    }
    readingDirection_ = direction;
    persistComicReadingProfile();
    emit viewStateChanged();
    if (isOpen()) {
        tryComposeAndEmitCurrentPage();
    }
}

void ReaderController::setFitMode(FitMode mode) {
    if (fitMode_ == mode) {
        return;
    }
    fitMode_ = mode;
    persistComicReadingProfile();
    emit viewStateChanged();
}

void ReaderController::setSmartWideSpread(bool enabled) {
    if (smartWideSpread_ == enabled) {
        return;
    }
    smartWideSpread_ = enabled;
    emit viewStateChanged();
    if (isOpen()) {
        requestCurrentAndPrefetchPages();
    }
}

void ReaderController::setAutoCropBorders(bool enabled) {
    if (autoCropBorders_ == enabled) {
        return;
    }
    autoCropBorders_ = enabled;
    emit viewStateChanged();
    if (isOpen()) {
        tryComposeAndEmitCurrentPage();
    }
}

void ReaderController::setZoomFactor(float zoom) {
    const float clamped = (std::clamp)(zoom, kMinZoomFactor, kMaxZoomFactor);
    if (clamped == zoomFactor_) {
        return;
    }
    zoomFactor_ = clamped;
    emit viewStateChanged();
}

void ReaderController::zoomIn() {
    setZoomFactor(zoomFactor_ * kZoomStepFactor);
}

void ReaderController::zoomOut() {
    setZoomFactor(zoomFactor_ / kZoomStepFactor);
}

void ReaderController::resetZoom() {
    setZoomFactor(1.0f);
}

void ReaderController::setRotation(int degrees) {
    const int normalized = RenderingEngine::normalizeRotation(degrees);
    if (normalized == rotationDegrees_) {
        return;
    }
    rotationDegrees_ = normalized;
    emit viewStateChanged();
}

void ReaderController::rotateClockwise() {
    setRotation(rotationDegrees_ + 90);
}

void ReaderController::rotateCounterClockwise() {
    setRotation(rotationDegrees_ - 90);
}

LayoutOptions ReaderController::currentLayoutOptions() const noexcept {
    LayoutOptions opts;
    opts.readingMode = readingMode_;
    opts.direction = readingDirection_;
    opts.smartWideSpread = smartWideSpread_;
    opts.autoCropBorders = autoCropBorders_;
    return opts;
}

Result<std::int64_t> ReaderController::addBookmark(
    std::string_view name,
    std::optional<std::size_t> pageIndex) {
    if (!isOpen()) {
        return Result<std::int64_t>::err(ErrorCode::InvalidArgument, "No comic is currently open");
    }
    if (name.empty()) {
        return Result<std::int64_t>::err(ErrorCode::InvalidArgument, "Bookmark name cannot be empty");
    }

    const std::size_t targetPage = (std::min)(pageIndex.value_or(currentPage_), totalPages_ - 1);
    const std::int64_t nowUnix = currentUnixSeconds();

    if (db_ != nullptr && db_->isOpen() && comicId_ > 0) {
        auto res = db_->addBookmark(
            comicId_,
            static_cast<std::uint32_t>(targetPage),
            name,
            nowUnix);
        if (!res.isOk()) {
            return res;
        }
        loadBookmarks();
        return res;
    }

    BookmarkRecord rec;
    rec.id = nextSyntheticBookmarkId_--;
    rec.comicId = comicId_;
    rec.pageIndex = static_cast<std::uint32_t>(targetPage);
    rec.name = std::string(name);
    rec.createdUnix = nowUnix;
    bookmarks_.push_back(std::move(rec));
    std::sort(bookmarks_.begin(), bookmarks_.end(), [](const BookmarkRecord& a, const BookmarkRecord& b) {
        if (a.pageIndex != b.pageIndex) {
            return a.pageIndex < b.pageIndex;
        }
        return a.id < b.id;
    });
    emit bookmarksChanged();
    return Result<std::int64_t>::ok(bookmarks_.back().id);
}

Result<void> ReaderController::removeBookmark(std::int64_t bookmarkId) {
    if (db_ != nullptr && db_->isOpen() && comicId_ > 0 && bookmarkId > 0) {
        auto res = db_->deleteBookmark(bookmarkId);
        if (!res.isOk()) {
            return res;
        }
        loadBookmarks();
        return Result<void>::ok();
    }

    const auto prevSize = bookmarks_.size();
    bookmarks_.erase(
        std::remove_if(bookmarks_.begin(), bookmarks_.end(), [bookmarkId](const BookmarkRecord& b) {
            return b.id == bookmarkId;
        }),
        bookmarks_.end());

    if (bookmarks_.size() == prevSize) {
        return Result<void>::err(ErrorCode::NotFound, "Bookmark not found");
    }

    emit bookmarksChanged();
    return Result<void>::ok();
}

bool ReaderController::goToBookmark(std::int64_t bookmarkId) {
    for (const auto& bm : bookmarks_) {
        if (bm.id == bookmarkId) {
            return goToPage(static_cast<std::size_t>(bm.pageIndex));
        }
    }
    return false;
}

void ReaderController::setProgressDebounceMs(int ms) {
    progressDebounceMs_ = (std::max)(0, ms);
    if (progressDebounceTimer_ != nullptr) {
        progressDebounceTimer_->setInterval(progressDebounceMs_);
    }
}

void ReaderController::flushReadingProgress() {
    if (!progressDirty_) {
        return;
    }
    progressDirty_ = false;

    if (db_ == nullptr || !db_->isOpen() || comicId_ <= 0 || totalPages_ == 0) {
        return;
    }

    const bool isRead = (currentPage_ + lastPagesConsumed_ >= totalPages_);
    auto res = db_->updateReadingProgress(
        comicId_,
        static_cast<std::uint32_t>(currentPage_),
        currentUnixSeconds(),
        isRead);
    if (!res.isOk()) {
        spdlog::warn("ReaderController: failed to persist reading progress for comic {}: {}",
                     comicId_,
                     res.error().message);
        emit errorOccurred(QString::fromStdString(res.error().message));
        return;
    }

    emit readingProgressSaved(comicId_, static_cast<int>(currentPage_), isRead);
}

bool ReaderController::isPageCached(std::size_t pageIndex) const {
    if (!isOpen()) {
        return false;
    }
    return cacheManager_->pageCache().contains(PageCacheKey{effectiveCacheComicId(), pageIndex});
}

std::shared_ptr<const ImageFrame> ReaderController::cachedPageFrame(std::size_t pageIndex) const {
    if (!isOpen()) {
        return nullptr;
    }
    return cacheManager_->pageCache().get(PageCacheKey{effectiveCacheComicId(), pageIndex});
}

std::size_t ReaderController::activeTaskCount() const {
    return inflightTasks_.size();
}

std::int64_t ReaderController::effectiveCacheComicId() const noexcept {
    return (comicId_ > 0) ? comicId_ : syntheticCacheId_;
}

std::vector<std::size_t> ReaderController::computeDesiredPages(std::size_t basePage) const {
    std::vector<std::size_t> pages;
    if (!isOpen()) {
        return pages;
    }

    pages.reserve(2 + kPrefetchLookahead);
    for (std::size_t offset = 0; offset <= kPrefetchLookahead; ++offset) {
        const std::size_t idx = basePage + offset;
        if (idx < totalPages_) {
            pages.push_back(idx);
        }
    }
    if (readingMode_ == ReadingMode::VerticalWebtoon && basePage > 0) {
        pages.push_back(basePage - 1);
    }
    return pages;
}

void ReaderController::requestCurrentAndPrefetchPages() {
    if (!isOpen() || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    const std::vector<std::size_t> desired = computeDesiredPages(currentPage_);
    std::unordered_set<std::size_t> desiredSet(desired.begin(), desired.end());

    // Immediately cancel any in-flight tasks outside the new [N, N+1, N+2] window.
    for (auto it = inflightTasks_.begin(); it != inflightTasks_.end();) {
        if (desiredSet.find(it->first) == desiredSet.end()) {
            const std::size_t cancelledPage = it->first;
            it->second.handle.cancel();
            cancelledTaskCount_.fetch_add(1, std::memory_order_relaxed);
            it = inflightTasks_.erase(it);
            emit pageTaskCancelled(static_cast<int>(cancelledPage));
        } else {
            ++it;
        }
    }

    const std::int64_t cacheComicId = effectiveCacheComicId();

    for (std::size_t i = 0; i < desired.size(); ++i) {
        const std::size_t pageIdx = desired[i];
        if (cacheManager_->pageCache().contains(PageCacheKey{cacheComicId, pageIdx})) {
            continue;
        }
        if (inflightTasks_.find(pageIdx) != inflightTasks_.end()) {
            continue;
        }

        RequestPriority prio = RequestPriority::Low;
        bool isPrefetch = true;
        if (i == 0) {
            prio = RequestPriority::Immediate;
            isPrefetch = false;
        } else if (i == 1) {
            prio = (readingMode_ == ReadingMode::DualPage)
                ? RequestPriority::High
                : RequestPriority::Normal;
            isPrefetch = (readingMode_ != ReadingMode::DualPage);
        } else {
            prio = RequestPriority::Low;
            isPrefetch = true;
        }

        enqueuePageDecode(pageIdx, prio, isPrefetch);
    }

    // If the required current page(s) are already in L1 RAM cache, compose immediately.
    tryComposeAndEmitCurrentPage();
}

void ReaderController::enqueuePageDecode(
    std::size_t pageIndex,
    RequestPriority priority,
    bool isPrefetch) {
    if (!isOpen() || pool_ == nullptr || shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    const std::shared_ptr<ReaderSession> session = session_;
    const std::int64_t cacheComicId = effectiveCacheComicId();
    const std::uint64_t gen = sessionGeneration_;
    const std::shared_ptr<std::atomic<bool>> alive = alive_;

    TaskHandle handle = pool_->enqueue(
        priority,
        [this, session, cacheComicId, pageIndex, gen, isPrefetch, alive](std::stop_token st) {
            {
                std::scoped_lock lock(workerWaitMutex_);
                ++runningWorkers_;
            }
            struct WorkerGuard {
                std::mutex& mtx;
                std::condition_variable& cv;
                std::size_t& count;
                ~WorkerGuard() {
                    {
                        std::scoped_lock lock(mtx);
                        --count;
                    }
                    cv.notify_all();
                }
            } guard{workerWaitMutex_, workerWaitCv_, runningWorkers_};

            if (st.stop_requested() || !alive->load(std::memory_order_acquire)) {
                return;
            }

            Result<ImageFrame> decodedRes = Result<ImageFrame>::err(
                ErrorCode::Unknown, "Uninitialized decode result");

            if (session->isPdf) {
                std::unique_lock ioLock(session->ioMutex);
                if (st.stop_requested() || !alive->load(std::memory_order_acquire)) {
                    return;
                }
                auto* pdfReader = static_cast<PdfDocumentReader*>(session->reader.get());
                decodedRes = pdfReader->renderPage(pageIndex, 0, 0, st);
            } else {
                Result<std::vector<std::uint8_t>> bytesRes = Result<std::vector<std::uint8_t>>::err(
                    ErrorCode::Unknown, "Uninitialized read result");
                {
                    std::unique_lock ioLock(session->ioMutex);
                    if (st.stop_requested() || !alive->load(std::memory_order_acquire)) {
                        return;
                    }
                    bytesRes = session->reader->readPageBytes(pageIndex, st);
                }

                if (st.stop_requested() || !alive->load(std::memory_order_acquire)) {
                    return;
                }

                if (!bytesRes.isOk()) {
                    decodedRes = Result<ImageFrame>::err(bytesRes.error());
                } else {
                    decodedRes = ImageDecoder::decode(bytesRes.value(), 0, 0, st);
                }
            }

            if (st.stop_requested() || !alive->load(std::memory_order_acquire)) {
                return;
            }

            if (!decodedRes.isOk()) {
                if (decodedRes.errorCode() == ErrorCode::Cancelled) {
                    return;
                }
                const std::string errMsg = decodedRes.error().message;
                QMetaObject::invokeMethod(
                    this,
                    [this, alive, gen, pageIndex, errMsg]() {
                        if (!alive->load(std::memory_order_acquire) || gen != sessionGeneration_) {
                            return;
                        }
                        inflightTasks_.erase(pageIndex);
                        emit errorOccurred(QString::fromStdString(errMsg));
                    },
                    Qt::QueuedConnection);
                return;
            }

            auto framePtr = std::make_shared<const ImageFrame>(std::move(decodedRes.value()));
            cacheManager_->pageCache().put(PageCacheKey{cacheComicId, pageIndex}, framePtr);
            decodedPageCount_.fetch_add(1, std::memory_order_relaxed);

            QMetaObject::invokeMethod(
                this,
                [this, alive, gen, pageIndex, isPrefetch]() {
                    if (!alive->load(std::memory_order_acquire) || gen != sessionGeneration_) {
                        return;
                    }
                    inflightTasks_.erase(pageIndex);

                    if (isPrefetch || pageIndex != currentPage_) {
                        emit pagePrefetched(static_cast<int>(pageIndex));
                    }

                    const bool affectsCurrent =
                        (pageIndex == currentPage_) ||
                        (readingMode_ == ReadingMode::DualPage && pageIndex == currentPage_ + 1) ||
                        (readingMode_ == ReadingMode::VerticalWebtoon &&
                         ((currentPage_ > 0 && pageIndex == currentPage_ - 1) ||
                          pageIndex == currentPage_ + 1));
                    if (affectsCurrent) {
                        tryComposeAndEmitCurrentPage();
                    }
                },
                Qt::QueuedConnection);
        });

    inflightTasks_.insert_or_assign(pageIndex, InFlightTask{std::move(handle), gen, isPrefetch});
}

void ReaderController::cancelAllInFlightTasks() {
    for (auto& [pageIdx, task] : inflightTasks_) {
        task.handle.cancel();
        cancelledTaskCount_.fetch_add(1, std::memory_order_relaxed);
    }
    inflightTasks_.clear();
}

void ReaderController::tryComposeAndEmitCurrentPage() {
    if (!isOpen()) {
        return;
    }

    const std::int64_t cacheComicId = effectiveCacheComicId();
    const auto primaryFrame = cacheManager_->pageCache().get(
        PageCacheKey{cacheComicId, currentPage_});
    if (primaryFrame == nullptr || !primaryFrame->isValid()) {
        return;
    }

    // Auto-detect ultra-tall Webtoon strips on first decoded page when no per-comic override exists.
    if (pendingAutoWebtoonDetect_) {
        pendingAutoWebtoonDetect_ = false;
        if (primaryFrame->width > 0 &&
            (static_cast<double>(primaryFrame->height) / static_cast<double>(primaryFrame->width)) > 2.2) {
            applyingProfileOrSettings_ = true;
            readingMode_ = ReadingMode::VerticalWebtoon;
            readingDirection_ = ReadingDirection::LTR;
            fitMode_ = FitMode::FitWidth;
            applyingProfileOrSettings_ = false;
            emit viewStateChanged();
            requestCurrentAndPrefetchPages();
            return;
        }
    }

    if (readingMode_ == ReadingMode::VerticalWebtoon) {
        // In VerticalWebtoon mode, compose only the single current page; ReaderViewport
        // maintains a per-page GPU texture cache for [N-1, N, N+1] without CPU stitching.
        auto layoutRes = RenderingEngine::composePageLayout(
            *primaryFrame,
            nullptr,
            currentLayoutOptions());
        if (!layoutRes.isOk()) {
            emit errorOccurred(QString::fromStdString(layoutRes.error().message));
            return;
        }
        lastPagesConsumed_ = layoutRes.value().pagesConsumed;
        currentLayout_ = std::move(layoutRes.value());
        emit pageReady(static_cast<int>(currentPage_), *currentLayout_);
        return;
    }

    std::shared_ptr<const ImageFrame> secondaryFrame{};
    if (readingMode_ == ReadingMode::DualPage) {
        const bool primaryIsWide = smartWideSpread_ && RenderingEngine::isWideSpread(*primaryFrame);
        if (!primaryIsWide && (currentPage_ + 1) < totalPages_) {
            secondaryFrame = cacheManager_->pageCache().get(
                PageCacheKey{cacheComicId, currentPage_ + 1});
            if (secondaryFrame == nullptr) {
                // Wait for page N+1 to finish decoding before emitting the dual spread.
                return;
            }
        }
    }

    auto layoutRes = RenderingEngine::composePageLayout(
        *primaryFrame,
        secondaryFrame.get(),
        currentLayoutOptions());
    if (!layoutRes.isOk()) {
        emit errorOccurred(QString::fromStdString(layoutRes.error().message));
        return;
    }

    lastPagesConsumed_ = layoutRes.value().pagesConsumed;
    currentLayout_ = std::move(layoutRes.value());
    emit pageReady(static_cast<int>(currentPage_), *currentLayout_);
}

void ReaderController::scheduleProgressSave() {
    if (!isOpen()) {
        return;
    }
    progressDirty_ = true;
    if (progressDebounceTimer_ != nullptr) {
        progressDebounceTimer_->start(progressDebounceMs_);
    }
}

void ReaderController::loadBookmarks() {
    bookmarks_.clear();
    if (db_ != nullptr && db_->isOpen() && comicId_ > 0) {
        auto res = db_->getBookmarksForComic(comicId_);
        if (res.isOk()) {
            bookmarks_ = std::move(res.value());
        } else {
            spdlog::warn("ReaderController: failed to load bookmarks for comic {}: {}",
                         comicId_,
                         res.error().message);
        }
    }
    emit bookmarksChanged();
}

} // namespace mavira::app
