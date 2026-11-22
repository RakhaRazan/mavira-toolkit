#include "app/ReaderController.hpp"
#include "core/archive/IArchiveReader.hpp"
#include "core/cache/CacheManager.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "core/rendering/RenderingEngine.hpp"
#include "core/threading/WorkerPool.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using mavira::app::ReaderController;
using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;
using mavira::core::archive::IArchiveReader;
using mavira::core::cache::CacheManager;
using mavira::core::imaging::ImageDecoder;
using mavira::core::rendering::AutoCropOptions;
using mavira::core::rendering::FitMode;
using mavira::core::rendering::LayoutOptions;
using mavira::core::rendering::ReadingDirection;
using mavira::core::rendering::ReadingMode;
using mavira::core::rendering::RenderingEngine;
using mavira::core::rendering::SpreadLayoutResult;
using mavira::core::threading::WorkerPool;
using mavira::data::db::ComicRecord;
using mavira::data::db::DatabaseManager;

ImageFrame makeSolidFrame(
    std::uint32_t width,
    std::uint32_t height,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    std::uint8_t a = 255) {
    ImageFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * ImageFrame::kBytesPerPixel;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * height);
    for (std::size_t i = 0; i < frame.pixels.size(); i += 4) {
        frame.pixels[i + 0] = r;
        frame.pixels[i + 1] = g;
        frame.pixels[i + 2] = b;
        frame.pixels[i + 3] = a;
    }
    return frame;
}

void fillRect(
    ImageFrame& frame,
    std::uint32_t rx,
    std::uint32_t ry,
    std::uint32_t rw,
    std::uint32_t rh,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    std::uint8_t a = 255) {
    const std::uint32_t stride = frame.effectiveStride();
    for (std::uint32_t y = ry; y < ry + rh && y < frame.height; ++y) {
        std::uint8_t* row = frame.pixels.data() + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = rx; x < rx + rw && x < frame.width; ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            px[0] = r;
            px[1] = g;
            px[2] = b;
            px[3] = a;
        }
    }
}

std::array<std::uint8_t, 4> pixelAt(const ImageFrame& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t idx =
        static_cast<std::size_t>(y) * frame.effectiveStride() + static_cast<std::size_t>(x) * 4;
    return {frame.pixels[idx], frame.pixels[idx + 1], frame.pixels[idx + 2], frame.pixels[idx + 3]};
}

void processEventsFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Pred>
bool waitUntil(Pred&& pred, int timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

/// Instrumented in-memory mock reader that encodes distinct synthetic JPEG pages
/// and optionally simulates slow I/O with cooperative `std::stop_token` cancellation.
class MockArchiveReader final : public IArchiveReader {
public:
    MockArchiveReader(
        std::size_t pageCount,
        int readDelayMs = 0,
        std::set<std::size_t> widePages = {},
        std::shared_ptr<std::atomic<bool>> closedFlag = nullptr)
        : readDelayMs_(readDelayMs),
          closedFlag_(std::move(closedFlag)) {
        pages_.reserve(pageCount);
        for (std::size_t i = 0; i < pageCount; ++i) {
            const bool isWide = widePages.count(i) > 0;
            const std::uint32_t w = isWide ? 160 : 80;
            const std::uint32_t h = isWide ? 100 : 120;
            const auto r = static_cast<std::uint8_t>((i * 37 + 40) % 256);
            const auto g = static_cast<std::uint8_t>((i * 73 + 80) % 256);
            const auto b = static_cast<std::uint8_t>((i * 19 + 120) % 256);
            ImageFrame frame = makeSolidFrame(w, h, r, g, b);
            auto encoded = ImageDecoder::encodeJpeg(frame, 90);
            EXPECT_TRUE(encoded.isOk());
            pages_.push_back(std::move(encoded.value()));
        }
        open_ = true;
    }

    Result<void> open(const std::filesystem::path&) override {
        open_ = true;
        return Result<void>::ok();
    }

    void close() noexcept override {
        open_ = false;
        if (closedFlag_ != nullptr) {
            closedFlag_->store(true, std::memory_order_release);
        }
    }

    bool isOpen() const noexcept override {
        return open_;
    }

    std::size_t pageCount() const noexcept override {
        return pages_.size();
    }

    std::string pageName(std::size_t index) const override {
        if (index >= pages_.size()) {
            return {};
        }
        return "page_" + std::to_string(index) + ".jpg";
    }

    Result<std::vector<std::uint8_t>> readPageBytes(
        std::size_t index,
        std::stop_token stopToken = {}) override {
        if (!open_ || index >= pages_.size()) {
            return Result<std::vector<std::uint8_t>>::err(ErrorCode::NotFound, "Out of range");
        }

        if (readDelayMs_ > 0) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(readDelayMs_);
            while (std::chrono::steady_clock::now() < deadline) {
                if (stopToken.stop_requested()) {
                    cancelledReads_.fetch_add(1, std::memory_order_relaxed);
                    return Result<std::vector<std::uint8_t>>::err(
                        ErrorCode::Cancelled, "Mock read cancelled");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        if (stopToken.stop_requested()) {
            cancelledReads_.fetch_add(1, std::memory_order_relaxed);
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::Cancelled, "Mock read cancelled");
        }

        {
            std::scoped_lock lock(statsMutex_);
            completedReadPages_.push_back(index);
        }
        return Result<std::vector<std::uint8_t>>::ok(pages_[index]);
    }

    Result<std::optional<std::string>> readMetadataXml() override {
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    std::vector<std::size_t> completedReadPages() const {
        std::scoped_lock lock(statsMutex_);
        return completedReadPages_;
    }

    std::uint64_t cancelledReads() const noexcept {
        return cancelledReads_.load(std::memory_order_relaxed);
    }

private:
    bool open_{false};
    int readDelayMs_{0};
    std::shared_ptr<std::atomic<bool>> closedFlag_{};
    std::vector<std::vector<std::uint8_t>> pages_{};
    mutable std::mutex statsMutex_{};
    std::vector<std::size_t> completedReadPages_{};
    std::atomic<std::uint64_t> cancelledReads_{0};
};

// =============================================================================
// RenderingEngine Tests
// =============================================================================

TEST(RenderingEngineTest, SmartPageLayoutRendersWideSpreadSoloInDualMode) {
    const ImageFrame widePrimary = makeSolidFrame(1600, 1000, 200, 100, 50);
    const ImageFrame portraitSecondary = makeSolidFrame(800, 1200, 50, 100, 200);

    LayoutOptions opts;
    opts.readingMode = ReadingMode::DualPage;
    opts.direction = ReadingDirection::RTL;
    opts.smartWideSpread = true;

    auto res = RenderingEngine::composePageLayout(widePrimary, &portraitSecondary, opts);
    ASSERT_TRUE(res.isOk());
    const SpreadLayoutResult& layout = res.value();

    EXPECT_FALSE(layout.isDualSpread);
    EXPECT_TRUE(layout.isWideSolo);
    EXPECT_EQ(layout.pagesConsumed, 1u);
    EXPECT_EQ(layout.composedFrame.width, 1600u);
    EXPECT_EQ(layout.composedFrame.height, 1000u);

    // When primary is portrait and secondary is wide, primary should also render solo
    // so the wide secondary gets its own full-width spread on the next page turn.
    auto res2 = RenderingEngine::composePageLayout(portraitSecondary, &widePrimary, opts);
    ASSERT_TRUE(res2.isOk());
    EXPECT_FALSE(res2.value().isDualSpread);
    EXPECT_EQ(res2.value().pagesConsumed, 1u);
    EXPECT_EQ(res2.value().composedFrame.width, 800u);
}

TEST(RenderingEngineTest, RtlMangaAndLtrComicSideBySideStitching) {
    const ImageFrame pageN = makeSolidFrame(100, 200, 255, 0, 0);     // Red
    const ImageFrame pageNPlus1 = makeSolidFrame(100, 160, 0, 0, 255); // Blue

    // LTR Comic: [Page N (Red) | gap | Page N+1 (Blue)]
    LayoutOptions ltrOpts;
    ltrOpts.readingMode = ReadingMode::DualPage;
    ltrOpts.direction = ReadingDirection::LTR;
    ltrOpts.dualPageGapPx = 10;

    auto ltrRes = RenderingEngine::composePageLayout(pageN, &pageNPlus1, ltrOpts);
    ASSERT_TRUE(ltrRes.isOk());
    EXPECT_TRUE(ltrRes.value().isDualSpread);
    EXPECT_EQ(ltrRes.value().pagesConsumed, 2u);
    EXPECT_EQ(ltrRes.value().composedFrame.width, 210u);
    EXPECT_EQ(ltrRes.value().composedFrame.height, 200u);

    // Left side is Red, gap is Black, Right side (vertically centered at y=20..179) is Blue
    EXPECT_EQ(pixelAt(ltrRes.value().composedFrame, 50, 100), (std::array<std::uint8_t, 4>{255, 0, 0, 255}));
    EXPECT_EQ(pixelAt(ltrRes.value().composedFrame, 105, 100), (std::array<std::uint8_t, 4>{0, 0, 0, 255}));
    EXPECT_EQ(pixelAt(ltrRes.value().composedFrame, 150, 100), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));

    // RTL Manga: [Page N+1 (Blue) | gap | Page N (Red)]
    LayoutOptions rtlOpts = ltrOpts;
    rtlOpts.direction = ReadingDirection::RTL;

    auto rtlRes = RenderingEngine::composePageLayout(pageN, &pageNPlus1, rtlOpts);
    ASSERT_TRUE(rtlRes.isOk());
    EXPECT_TRUE(rtlRes.value().isDualSpread);
    EXPECT_EQ(rtlRes.value().pagesConsumed, 2u);
    EXPECT_EQ(pixelAt(rtlRes.value().composedFrame, 50, 100), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
    EXPECT_EQ(pixelAt(rtlRes.value().composedFrame, 150, 100), (std::array<std::uint8_t, 4>{255, 0, 0, 255}));
}

TEST(RenderingEngineTest, AutoCropTrimsUniformWhiteAndBlackBorders) {
    // 200x300 white page with a 100x160 green artwork block at (40, 60)
    ImageFrame whiteMarginPage = makeSolidFrame(200, 300, 255, 255, 255);
    fillRect(whiteMarginPage, 40, 60, 100, 160, 30, 180, 60);

    const ImageFrame croppedWhite = RenderingEngine::autoCropBorders(whiteMarginPage);
    EXPECT_EQ(croppedWhite.width, 100u);
    EXPECT_EQ(croppedWhite.height, 160u);
    EXPECT_EQ(pixelAt(croppedWhite, 0, 0), (std::array<std::uint8_t, 4>{30, 180, 60, 255}));

    // 200x300 black page with an 80x120 orange artwork block at (50, 70)
    ImageFrame blackMarginPage = makeSolidFrame(200, 300, 4, 4, 4);
    fillRect(blackMarginPage, 50, 70, 80, 120, 220, 120, 20);

    const ImageFrame croppedBlack = RenderingEngine::autoCropBorders(blackMarginPage);
    EXPECT_EQ(croppedBlack.width, 80u);
    EXPECT_EQ(croppedBlack.height, 120u);
    EXPECT_EQ(pixelAt(croppedBlack, 79, 119), (std::array<std::uint8_t, 4>{220, 120, 20, 255}));

    // Pure blank page must NOT collapse to 0x0.
    const ImageFrame blankPage = makeSolidFrame(200, 300, 255, 255, 255);
    const ImageFrame preserved = RenderingEngine::autoCropBorders(blankPage);
    EXPECT_EQ(preserved.width, 200u);
    EXPECT_EQ(preserved.height, 300u);
}

TEST(RenderingEngineTest, WebtoonTextureTileSplittingExceeding4096Px) {
    // 64 x 5500 ultra-tall Webtoon strip (> 4096 px) with distinct colors per 2048 px band.
    ImageFrame strip = makeSolidFrame(64, 5500, 0, 0, 0);
    fillRect(strip, 0, 0, 64, 2048, 200, 10, 10);
    fillRect(strip, 0, 2048, 64, 2048, 10, 200, 10);
    fillRect(strip, 0, 4096, 64, 1404, 10, 10, 200);

    LayoutOptions opts;
    opts.readingMode = ReadingMode::VerticalWebtoon;

    auto res = RenderingEngine::composePageLayout(strip, nullptr, opts);
    ASSERT_TRUE(res.isOk());
    const SpreadLayoutResult& layout = res.value();

    EXPECT_TRUE(layout.isTiled());
    ASSERT_EQ(layout.tiles.size(), 3u);

    EXPECT_EQ(layout.tiles[0].tileIndex, 0u);
    EXPECT_EQ(layout.tiles[0].offsetY, 0u);
    EXPECT_EQ(layout.tiles[0].frame.height, 2048u);
    EXPECT_EQ(pixelAt(layout.tiles[0].frame, 32, 100), (std::array<std::uint8_t, 4>{200, 10, 10, 255}));

    EXPECT_EQ(layout.tiles[1].tileIndex, 1u);
    EXPECT_EQ(layout.tiles[1].offsetY, 2048u);
    EXPECT_EQ(layout.tiles[1].frame.height, 2048u);
    EXPECT_EQ(pixelAt(layout.tiles[1].frame, 32, 100), (std::array<std::uint8_t, 4>{10, 200, 10, 255}));

    EXPECT_EQ(layout.tiles[2].tileIndex, 2u);
    EXPECT_EQ(layout.tiles[2].offsetY, 4096u);
    EXPECT_EQ(layout.tiles[2].frame.height, 1404u);
    EXPECT_EQ(pixelAt(layout.tiles[2].frame, 32, 100), (std::array<std::uint8_t, 4>{10, 10, 200, 255}));

    // Normal-height page (<= 4096 px) stays a single tile.
    const ImageFrame normalPage = makeSolidFrame(64, 4096, 50, 50, 50);
    auto normalRes = RenderingEngine::composePageLayout(normalPage, nullptr, opts);
    ASSERT_TRUE(normalRes.isOk());
    EXPECT_FALSE(normalRes.value().isTiled());
    ASSERT_EQ(normalRes.value().tiles.size(), 1u);
    EXPECT_EQ(normalRes.value().tiles[0].frame.height, 4096u);
}

// =============================================================================
// ReaderController Tests
// =============================================================================

class ReaderControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempRoot_ = std::filesystem::temp_directory_path() /
                    (L"mavira_reader_test_" + std::to_wstring(stamp));
        std::filesystem::create_directories(tempRoot_);

        const auto dbPath = tempRoot_ / L"library.db";
        ASSERT_TRUE(db_.open(dbPath).isOk());
        cache_ = std::make_unique<CacheManager>();
        pool_ = std::make_unique<WorkerPool>(2);
    }

    void TearDown() override {
        pool_->shutdown();
        pool_.reset();
        cache_.reset();
        db_.close();
        std::error_code ec;
        std::filesystem::remove_all(tempRoot_, ec);
    }

    std::filesystem::path tempRoot_{};
    DatabaseManager db_{};
    std::unique_ptr<CacheManager> cache_{};
    std::unique_ptr<WorkerPool> pool_{};
};

TEST_F(ReaderControllerTest, PrefetchesTwoPagesAheadAndServesFromCache) {
    auto mockReader = std::make_unique<MockArchiveReader>(10, /*readDelayMs=*/0);
    MockArchiveReader* rawReader = mockReader.get();

    ReaderController controller(&db_, cache_.get(), pool_.get());

    std::vector<int> readyPages;
    std::vector<int> prefetchedPages;
    QObject::connect(&controller, &ReaderController::pageReady, [&](int pageIdx, const SpreadLayoutResult& layout) {
        EXPECT_TRUE(layout.isValid());
        readyPages.push_back(pageIdx);
    });
    QObject::connect(&controller, &ReaderController::pagePrefetched, [&](int pageIdx) {
        prefetchedPages.push_back(pageIdx);
    });

    ASSERT_TRUE(controller.openReader(std::move(mockReader), /*comicId=*/0, /*startPage=*/0).isOk());

    // Wait for page 0 + 2-page lookahead (1, 2) to populate L1 RAM cache.
    ASSERT_TRUE(waitUntil([&]() {
        return controller.isPageCached(0) &&
               controller.isPageCached(1) &&
               controller.isPageCached(2) &&
               controller.activeTaskCount() == 0;
    }));

    EXPECT_FALSE(controller.isPageCached(3));
    EXPECT_FALSE(readyPages.empty());
    EXPECT_EQ(readyPages.back(), 0);

    // Turning to page 1 should serve page 1 immediately from cache and prefetch page 3 (N+2).
    const std::size_t readyBefore = readyPages.size();
    ASSERT_TRUE(controller.nextPage());
    EXPECT_EQ(controller.currentPage(), 1u);
    EXPECT_GT(readyPages.size(), readyBefore);
    EXPECT_EQ(readyPages.back(), 1);

    ASSERT_TRUE(waitUntil([&]() {
        return controller.isPageCached(3) && controller.activeTaskCount() == 0;
    }));
    EXPECT_FALSE(controller.isPageCached(4));
    EXPECT_EQ(rawReader->completedReadPages().size(), 4u);

    controller.shutdown();
}

TEST_F(ReaderControllerTest, RapidPageTurnsCancelObsoleteTasksCleanly) {
    // Use a 1-worker pool with 80ms read delay so queued lookahead pages stay pending
    // and in-flight reads can be cancelled cooperatively during rapid page turns.
    WorkerPool singleWorkerPool(1);
    auto mockReader = std::make_unique<MockArchiveReader>(20, /*readDelayMs=*/80);
    MockArchiveReader* rawReader = mockReader.get();

    ReaderController controller(&db_, cache_.get(), &singleWorkerPool);

    std::vector<int> cancelledPages;
    int lastReadyPage = -1;
    QObject::connect(&controller, &ReaderController::pageTaskCancelled, [&](int pageIdx) {
        cancelledPages.push_back(pageIdx);
    });
    QObject::connect(&controller, &ReaderController::pageReady, [&](int pageIdx, const SpreadLayoutResult&) {
        lastReadyPage = pageIdx;
    });

    ASSERT_TRUE(controller.openReader(std::move(mockReader), /*comicId=*/0, /*startPage=*/0).isOk());

    // Immediately fire rapid page jumps across the volume before the slow reads complete.
    ASSERT_TRUE(controller.goToPage(4));
    ASSERT_TRUE(controller.goToPage(9));
    ASSERT_TRUE(controller.goToPage(15));

    // Obsolete tasks from pages {0,1,2}, {4,5,6}, {9,10,11} must have been cancelled immediately.
    EXPECT_GE(controller.cancelledTaskCount(), 6u);
    EXPECT_GE(cancelledPages.size(), 6u);

    // Wait for the final target window {15, 16, 17} to finish decoding.
    ASSERT_TRUE(waitUntil([&]() {
        return lastReadyPage == 15 &&
               controller.isPageCached(15) &&
               controller.isPageCached(16) &&
               controller.isPageCached(17) &&
               controller.activeTaskCount() == 0;
    }, 5000));

    // Skipped intermediate pages should NOT have been decoded into the cache.
    EXPECT_FALSE(controller.isPageCached(4));
    EXPECT_FALSE(controller.isPageCached(5));
    EXPECT_FALSE(controller.isPageCached(9));
    EXPECT_FALSE(controller.isPageCached(10));

    // Total completed reads should be far fewer than the 12 pages that were queued.
    EXPECT_LE(rawReader->completedReadPages().size(), 4u);

    controller.shutdown();
    singleWorkerPool.shutdown();
}

TEST_F(ReaderControllerTest, DebouncesSqliteReadingProgressAndPersistsBookmarks) {
    ComicRecord rec;
    rec.filePath = tempRoot_ / L"test_manga.cbz";
    rec.folderPath = tempRoot_;
    rec.title = "Test Manga";
    rec.format = ComicFormat::CBZ;
    rec.pageCount = 10;
    rec.currentPage = 0;

    auto idRes = db_.upsertComic(rec);
    ASSERT_TRUE(idRes.isOk());
    const std::int64_t comicId = idRes.value();

    auto mockReader = std::make_unique<MockArchiveReader>(10, /*readDelayMs=*/0);
    ReaderController controller(&db_, cache_.get(), pool_.get());
    EXPECT_EQ(controller.progressDebounceMs(), 500);

    int savedSignalCount = 0;
    int lastSavedPage = -1;
    QObject::connect(&controller, &ReaderController::readingProgressSaved,
                     [&](qint64 id, int pageIdx, bool) {
                         EXPECT_EQ(id, comicId);
                         ++savedSignalCount;
                         lastSavedPage = pageIdx;
                     });

    ASSERT_TRUE(controller.openReader(std::move(mockReader), comicId, /*startPage=*/0).isOk());

    // Rapid page turns within 100 ms should coalesce into a single debounced SQLite write.
    ASSERT_TRUE(controller.goToPage(1));
    processEventsFor(30);
    ASSERT_TRUE(controller.goToPage(2));
    processEventsFor(30);
    ASSERT_TRUE(controller.goToPage(5));

    // Before 500 ms debounce elapses, DB must still show page 0.
    EXPECT_EQ(savedSignalCount, 0);
    auto beforeDb = db_.getComicById(comicId);
    ASSERT_TRUE(beforeDb.isOk() && beforeDb.value().has_value());
    EXPECT_EQ(beforeDb.value()->currentPage, 0u);

    // Wait past the 500 ms debounce interval.
    ASSERT_TRUE(waitUntil([&]() { return savedSignalCount == 1; }, 2000));
    EXPECT_EQ(lastSavedPage, 5);

    auto afterDb = db_.getComicById(comicId);
    ASSERT_TRUE(afterDb.isOk() && afterDb.value().has_value());
    EXPECT_EQ(afterDb.value()->currentPage, 5u);
    EXPECT_FALSE(afterDb.value()->isRead);

    // Verify View State controls (mode, fit, zoom, rotation).
    controller.setReadingMode(ReadingMode::DualPage);
    controller.setFitMode(FitMode::FitWidth);
    controller.zoomIn();
    EXPECT_GT(controller.zoomFactor(), 1.0f);
    controller.resetZoom();
    EXPECT_FLOAT_EQ(controller.zoomFactor(), 1.0f);
    controller.rotateClockwise();
    EXPECT_EQ(controller.rotationDegrees(), 90);
    controller.rotateCounterClockwise();
    EXPECT_EQ(controller.rotationDegrees(), 0);

    // Verify Named Bookmarks persisted in SQLite.
    auto bmRes = controller.addBookmark("Boss Fight");
    ASSERT_TRUE(bmRes.isOk());
    ASSERT_EQ(controller.bookmarks().size(), 1u);
    EXPECT_EQ(controller.bookmarks()[0].name, "Boss Fight");
    EXPECT_EQ(controller.bookmarks()[0].pageIndex, 5u);

    ASSERT_TRUE(controller.goToPage(1));
    EXPECT_EQ(controller.currentPage(), 1u);
    ASSERT_TRUE(controller.goToBookmark(bmRes.value()));
    EXPECT_EQ(controller.currentPage(), 5u);

    ASSERT_TRUE(controller.removeBookmark(bmRes.value()).isOk());
    EXPECT_TRUE(controller.bookmarks().empty());

    controller.shutdown();
}

TEST_F(ReaderControllerTest, CloseIfMatchingReleasesReaderEvictsCacheAndEmitsActiveComicClosed) {
    const std::filesystem::path openPath = tempRoot_ / L"deleted_while_open.cbz";
    const std::filesystem::path otherPath = tempRoot_ / L"other_comic.cbz";
    constexpr std::int64_t openId = 42;

    auto readerClosed = std::make_shared<std::atomic<bool>>(false);
    auto mockReader = std::make_unique<MockArchiveReader>(
        5, /*readDelayMs=*/0, std::set<std::size_t>{}, readerClosed);
    ReaderController controller(&db_, cache_.get(), pool_.get());

    int activeClosedCount = 0;
    QObject::connect(&controller, &ReaderController::activeComicClosed, [&]() {
        ++activeClosedCount;
    });

    ASSERT_TRUE(controller.openReader(std::move(mockReader), openId, /*startPage=*/0, openPath).isOk());
    ASSERT_TRUE(waitUntil([&]() {
        return controller.isPageCached(0) &&
               controller.isPageCached(1) &&
               controller.isPageCached(2) &&
               controller.activeTaskCount() == 0;
    }));
    EXPECT_GT(cache_->pageCache().entryCount(), 0u);
    EXPECT_FALSE(readerClosed->load(std::memory_order_acquire));

    // Non-matching ID and path must leave the active session untouched.
    EXPECT_FALSE(controller.closeIfMatching(999, otherPath.native()));
    EXPECT_TRUE(controller.isOpen());
    EXPECT_FALSE(readerClosed->load(std::memory_order_acquire));
    EXPECT_EQ(activeClosedCount, 0);

    // Matching ID closes immediately, evicts pages from CacheManager, and emits activeComicClosed().
    EXPECT_TRUE(controller.closeIfMatching(openId, openPath.native()));
    EXPECT_FALSE(controller.isOpen());
    EXPECT_TRUE(readerClosed->load(std::memory_order_acquire));
    EXPECT_EQ(controller.comicId(), 0);
    EXPECT_EQ(controller.totalPages(), 0u);
    EXPECT_TRUE(controller.currentPath().empty());
    EXPECT_FALSE(controller.currentLayout().has_value());
    EXPECT_EQ(cache_->pageCache().entryCount(), 0u);
    EXPECT_EQ(activeClosedCount, 1);

    // Also verify matching by filePath when comicId == 0.
    auto mockReader2 = std::make_unique<MockArchiveReader>(3, /*readDelayMs=*/0);
    ASSERT_TRUE(controller.openReader(std::move(mockReader2), 0, /*startPage=*/0, openPath).isOk());
    ASSERT_TRUE(waitUntil([&]() { return controller.isPageCached(0); }));
    EXPECT_TRUE(controller.closeIfMatching(0, openPath.native()));
    EXPECT_FALSE(controller.isOpen());
    EXPECT_EQ(cache_->pageCache().entryCount(), 0u);
    EXPECT_EQ(activeClosedCount, 2);

    controller.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
