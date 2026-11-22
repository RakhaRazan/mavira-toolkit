#include "core/cache/CacheManager.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "core/threading/WorkerPool.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

#include <gtest/gtest.h>
#include <zip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <latch>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::RequestPriority;
using mavira::core::cache::CacheManager;
using mavira::core::cache::ICoverStorage;
using mavira::core::cache::PackFileCoverProvider;
using mavira::core::cache::PageCacheKey;
using mavira::core::cache::PageRamCache;
using mavira::core::cache::RamOnlyCoverProvider;
using mavira::core::imaging::ImageDecoder;
using mavira::core::threading::WorkerPool;
using mavira::data::db::ComicRecord;
using mavira::data::db::DatabaseManager;
using mavira::data::fs::FileManager;

struct TempDirGuard {
    std::filesystem::path path;

    explicit TempDirGuard(const std::wstring& name) {
        path = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::shared_ptr<ImageFrame> makeSolidFrame(
    std::uint32_t width,
    std::uint32_t height,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b) {
    auto frame = std::make_shared<ImageFrame>();
    frame->width = width;
    frame->height = height;
    frame->stride = width * 4u;
    frame->pixels.resize(static_cast<std::size_t>(frame->stride) * height);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        frame->pixels[i * 4 + 0] = r;
        frame->pixels[i * 4 + 1] = g;
        frame->pixels[i * 4 + 2] = b;
        frame->pixels[i * 4 + 3] = 255;
    }
    return frame;
}

void createSingleImageCbz(const std::filesystem::path& cbzPath, const std::vector<std::uint8_t>& imgBytes) {
    std::filesystem::create_directories(cbzPath.parent_path());
    const std::wstring wide = FileManager::toWin32LongPath(cbzPath);

    zip_error_t err;
    ::zip_error_init(&err);
    zip_source_t* src = ::zip_source_win32w_create(wide.c_str(), 0, -1, &err);
    ASSERT_NE(src, nullptr) << ::zip_error_strerror(&err);
    zip_t* za = ::zip_open_from_source(src, ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (za == nullptr) {
        ::zip_source_free(src);
        FAIL() << ::zip_error_strerror(&err);
    }
    ::zip_error_fini(&err);

    zip_source_t* buf = ::zip_source_buffer(za, imgBytes.data(), imgBytes.size(), 0);
    ASSERT_NE(buf, nullptr);
    ASSERT_GE(::zip_file_add(za, "001_cover.jpg", buf, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE), 0);
    ASSERT_EQ(::zip_close(za), 0);
}

TEST(WorkerPoolTest, EnforcesWorkerCapPriorityOrderingAndCancellation) {
    WorkerPool defaultPool;
    EXPECT_GE(defaultPool.workerCount(), 1u);
    EXPECT_LE(defaultPool.workerCount(), WorkerPool::kMaxWorkers);

    // Single-worker pool to deterministically verify priority ordering and pre-start cancellation.
    WorkerPool singlePool(1);
    ASSERT_EQ(singlePool.workerCount(), 1u);

    std::latch blockerEntered(1);
    std::latch releaseBlocker(1);
    singlePool.enqueue(RequestPriority::Immediate, [&](std::stop_token) {
        blockerEntered.count_down();
        releaseBlocker.wait();
    });
    blockerEntered.wait();

    std::vector<int> executionOrder;
    std::mutex orderMutex;
    std::atomic<bool> cancelledTaskRan{false};

    auto [hLow, fLow] = singlePool.submit(RequestPriority::Low, [&](std::stop_token) {
        std::scoped_lock lock(orderMutex);
        executionOrder.push_back(1);
        return 1;
    });
    auto [hCancelled, fCancelled] = singlePool.submit(RequestPriority::High, [&](std::stop_token) {
        cancelledTaskRan.store(true);
        return 2;
    });
    auto [hImmediate, fImmediate] = singlePool.submit(RequestPriority::Immediate, [&](std::stop_token) {
        std::scoped_lock lock(orderMutex);
        executionOrder.push_back(3);
        return 3;
    });

    hCancelled.cancel();
    releaseBlocker.count_down();

    EXPECT_EQ(fImmediate.get(), 3);
    EXPECT_EQ(fLow.get(), 1);
    EXPECT_FALSE(cancelledTaskRan.load());

    ASSERT_EQ(executionOrder.size(), 2u);
    EXPECT_EQ(executionOrder[0], 3); // Immediate executed before Low
    EXPECT_EQ(executionOrder[1], 1);
}

TEST(PageRamCacheTest, EvictsPagesFurthestFromCurrentReadingIndexFirst) {
    // Each 100x100 RGBA frame is 40,000 bytes. Budget of 120,000 bytes holds at most 3 pages.
    constexpr std::size_t kFrameBytes = 100u * 100u * 4u;
    PageRamCache cache(kFrameBytes * 3u);

    cache.setCurrentReadingPosition(1, 10);

    // Insert pages 1, 9, 11 (distances from 10: 9, 1, 1)
    cache.put(PageCacheKey{1, 1}, makeSolidFrame(100, 100, 10, 0, 0));
    cache.put(PageCacheKey{1, 9}, makeSolidFrame(100, 100, 90, 0, 0));
    cache.put(PageCacheKey{1, 11}, makeSolidFrame(100, 100, 110, 0, 0));
    EXPECT_EQ(cache.entryCount(), 3u);
    EXPECT_EQ(cache.memoryUsageBytes(), kFrameBytes * 3u);

    // Inserting page 10 (distance 0) must evict page 1 (distance 9), keeping 9, 10, 11!
    cache.put(PageCacheKey{1, 10}, makeSolidFrame(100, 100, 100, 0, 0));
    EXPECT_EQ(cache.entryCount(), 3u);
    EXPECT_FALSE(cache.contains(PageCacheKey{1, 1}));
    EXPECT_TRUE(cache.contains(PageCacheKey{1, 9}));
    EXPECT_TRUE(cache.contains(PageCacheKey{1, 10}));
    EXPECT_TRUE(cache.contains(PageCacheKey{1, 11}));

    // Switching current comic to comicId=2 and inserting a page evicts comic 1 pages first.
    cache.setCurrentReadingPosition(2, 0);
    cache.put(PageCacheKey{2, 0}, makeSolidFrame(100, 100, 200, 0, 0));
    EXPECT_TRUE(cache.contains(PageCacheKey{2, 0}));
    EXPECT_EQ(cache.entryCount(), 3u);
}

TEST(CoverStorageTest, Mode1RamOnlyAndMode2PackFileRoundTrip) {
    TempDirGuard tmp(L"mavira_test_cover_storage");
    const std::filesystem::path dataDir = tmp.path / L"data";
    const std::filesystem::path dbPath = dataDir / L"library.db";
    const std::filesystem::path packPath = dataDir / L"covers.pack";

    // Create 3 test CBZ archives with distinct colored JPEG covers.
    auto jpg1 = ImageDecoder::encodeJpeg(*makeSolidFrame(360, 510, 220, 40, 40), 85);
    auto jpg2 = ImageDecoder::encodeJpeg(*makeSolidFrame(360, 510, 40, 220, 40), 85);
    auto jpg3 = ImageDecoder::encodeJpeg(*makeSolidFrame(360, 510, 40, 40, 220), 85);
    ASSERT_TRUE(jpg1.isOk() && jpg2.isOk() && jpg3.isOk());

    const std::filesystem::path cbz1 = tmp.path / L"comics" / L"Alpha_Vol1.cbz";
    const std::filesystem::path cbz2 = tmp.path / L"comics" / L"Beta_Vol2.cbz";
    const std::filesystem::path cbz3 = tmp.path / L"comics" / L"Gamma_Vol3.cbz";
    createSingleImageCbz(cbz1, jpg1.value());
    createSingleImageCbz(cbz2, jpg2.value());
    createSingleImageCbz(cbz3, jpg3.value());

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath).isOk());

    auto insertComic = [&](const std::filesystem::path& p, const std::string& title) {
        ComicRecord rec;
        rec.filePath = p;
        rec.folderPath = p.parent_path();
        rec.title = title;
        rec.format = ComicFormat::CBZ;
        auto idRes = db.upsertComic(rec);
        EXPECT_TRUE(idRes.isOk());
        rec.id = idRes.value();
        return rec;
    };

    ComicRecord c1 = insertComic(cbz1, "Alpha Vol 1");
    ComicRecord c2 = insertComic(cbz2, "Beta Vol 2");
    ComicRecord c3 = insertComic(cbz3, "Gamma Vol 3");

    // -------------------------------------------------------------------------
    // Mode 1: RamOnlyCoverProvider (0 MB disk, instant stop_token cancellation)
    // -------------------------------------------------------------------------
    RamOnlyCoverProvider ramProvider;
    {
        std::stop_source ss;
        ss.request_stop();
        auto cancelled = ramProvider.getCover(c1, ss.get_token());
        EXPECT_FALSE(cancelled.isOk());
        EXPECT_EQ(cancelled.errorCode(), ErrorCode::Cancelled);
    }

    auto ramCover1 = ramProvider.getCover(c1);
    ASSERT_TRUE(ramCover1.isOk()) << ramCover1.error().message;
    EXPECT_EQ(ramCover1.value()->width, ICoverStorage::kCoverMaxWidth);
    EXPECT_EQ(ramCover1.value()->height, ICoverStorage::kCoverMaxHeight);
    EXPECT_GT(ramProvider.memoryUsageBytes(), 0u);
    EXPECT_FALSE(std::filesystem::exists(packPath)); // Mode 1 writes 0 bytes to disk!

    // -------------------------------------------------------------------------
    // Mode 2: PackFileCoverProvider (single-file covers.pack + SQLite offsets)
    // -------------------------------------------------------------------------
    PackFileCoverProvider packProvider(packPath, &db);
    auto indexedRes = packProvider.indexUncachedCovers();
    ASSERT_TRUE(indexedRes.isOk()) << indexedRes.error().message;
    EXPECT_EQ(indexedRes.value(), 3u);
    EXPECT_TRUE(std::filesystem::exists(packPath));
    EXPECT_GT(std::filesystem::file_size(packPath), 0u);

    // Reload records from SQLite and verify (cover_pack_offset, cover_pack_size) were persisted.
    auto dbC1 = db.getComicById(c1.id);
    auto dbC2 = db.getComicById(c2.id);
    auto dbC3 = db.getComicById(c3.id);
    ASSERT_TRUE(dbC1.isOk() && dbC1.value().has_value());
    ASSERT_TRUE(dbC2.isOk() && dbC2.value().has_value());
    ASSERT_TRUE(dbC3.isOk() && dbC3.value().has_value());

    EXPECT_EQ(dbC1.value()->coverPackOffset, 0);
    EXPECT_GT(dbC1.value()->coverPackSize, 0u);
    EXPECT_EQ(dbC2.value()->coverPackOffset, static_cast<std::int64_t>(dbC1.value()->coverPackSize));
    EXPECT_GT(dbC2.value()->coverPackSize, 0u);
    EXPECT_EQ(
        dbC3.value()->coverPackOffset,
        dbC2.value()->coverPackOffset + static_cast<std::int64_t>(dbC2.value()->coverPackSize));
    EXPECT_GT(dbC3.value()->coverPackSize, 0u);

    // Verify raw WebP payload read directly by (offset, size) from covers.pack.
    auto rawWebp2 = packProvider.readRawWebpFromPack(
        dbC2.value()->coverPackOffset, dbC2.value()->coverPackSize);
    ASSERT_TRUE(rawWebp2.isOk());
    EXPECT_EQ(ImageDecoder::detectFormat(rawWebp2.value()), mavira::core::imaging::ImageFormat::WEBP);

    // Delete the original CBZ file to prove getCover() reads purely from covers.pack!
    std::filesystem::remove(cbz2);
    packProvider.clearMemoryCache();
    auto packCover2 = packProvider.getCover(*dbC2.value());
    ASSERT_TRUE(packCover2.isOk()) << packCover2.error().message;
    EXPECT_EQ(packCover2.value()->width, ICoverStorage::kCoverMaxWidth);
    EXPECT_EQ(packCover2.value()->height, ICoverStorage::kCoverMaxHeight);
    EXPECT_TRUE(packCover2.value()->isValid());

    // Re-running indexUncachedCovers() finds 0 uncached covers.
    auto secondIndexRes = packProvider.indexUncachedCovers();
    ASSERT_TRUE(secondIndexRes.isOk());
    EXPECT_EQ(secondIndexRes.value(), 0u);
}

} // namespace
