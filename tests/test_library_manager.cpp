#include "app/LibraryManager.hpp"
#include "core/cache/CacheManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

#include <Windows.h>
#include <gtest/gtest.h>
#include <zip.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Minimal valid 1x1 PNG bytes so ImageDecoder / ArchiveEngine treat pages as real images.
constexpr std::uint8_t kTinyPngBytes[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
    0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99,
    0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

void createTestCbz(
    const std::filesystem::path& cbzPath,
    int pageCount,
    const std::string& comicInfoXml = {}) {
    std::filesystem::create_directories(cbzPath.parent_path());
    int errCode = 0;
    const std::string utf8Path = mavira::data::fs::FileManager::wideToUtf8(cbzPath.native());
    zip_t* archive = zip_open(utf8Path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errCode);
    ASSERT_NE(archive, nullptr) << "zip_open failed with code " << errCode;

    for (int i = 1; i <= pageCount; ++i) {
        const std::string entryName = "page_" + std::to_string(i) + ".png";
        zip_source_t* src = zip_source_buffer(archive, kTinyPngBytes, sizeof(kTinyPngBytes), 0);
        ASSERT_NE(src, nullptr);
        ASSERT_GE(zip_file_add(archive, entryName.c_str(), src, ZIP_FL_OVERWRITE), 0);
    }

    if (!comicInfoXml.empty()) {
        zip_source_t* xmlSrc = zip_source_buffer(
            archive, comicInfoXml.data(), comicInfoXml.size(), 0);
        ASSERT_NE(xmlSrc, nullptr);
        ASSERT_GE(zip_file_add(archive, "ComicInfo.xml", xmlSrc, ZIP_FL_OVERWRITE), 0);
    }

    ASSERT_EQ(zip_close(archive), 0);
}

void writeTinyPngFile(const std::filesystem::path& filePath) {
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(reinterpret_cast<const char*>(kTinyPngBytes), sizeof(kTinyPngBytes));
    ASSERT_TRUE(out.good());
}

class LibraryManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto uniqueStamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempRoot_ = std::filesystem::temp_directory_path() /
                    (L"mavira_libmgr_test_" + std::to_wstring(uniqueStamp));
        std::filesystem::create_directories(tempRoot_);

        portablePaths_ = mavira::data::fs::FileManager::resolvePortablePaths(tempRoot_);
        ASSERT_TRUE(mavira::data::fs::FileManager::ensurePortableDirectories(portablePaths_).isOk());

        ASSERT_TRUE(db_.open(portablePaths_.databasePath).isOk());
        cache_ = std::make_unique<mavira::core::cache::CacheManager>();
    }

    void TearDown() override {
        cache_.reset();
        db_.close();
        std::error_code ec;
        std::filesystem::remove_all(tempRoot_, ec);
    }

    std::filesystem::path tempRoot_{};
    mavira::data::fs::PortablePaths portablePaths_{};
    mavira::data::db::DatabaseManager db_{};
    std::unique_ptr<mavira::core::cache::CacheManager> cache_{};
};

TEST_F(LibraryManagerTest, BackgroundScanIndexesNestedFoldersWithoutFreezingEventLoop) {
    const std::filesystem::path libDir = tempRoot_ / L"ComicsRoot";
    const std::filesystem::path nestedA = libDir / L"Action" / L"SeriesA";
    const std::filesystem::path nestedB = libDir / L"SciFi" / L"SeriesB" / L"Arc1";
    const std::filesystem::path rawFolder = libDir / L"Artbooks" / L"RawChapter";

    const std::string xml =
        "<ComicInfo>"
        "<Title>Neon Horizon Vol 1</Title>"
        "<Series>Neon Horizon</Series>"
        "<Writer>A. Author</Writer>"
        "<Tags>Cyberpunk, Action</Tags>"
        "</ComicInfo>";

    createTestCbz(nestedA / L"Vol01.cbz", 3, xml);
    createTestCbz(nestedA / L"[StudioX] Vol02 (C100).cbz", 2);
    createTestCbz(nestedB / L"Chapter01.cbz", 4);
    writeTinyPngFile(rawFolder / L"001.png");
    writeTinyPngFile(rawFolder / L"002.png");

    mavira::app::LibraryManager mgr(&db_, cache_.get(), portablePaths_);
    mgr.setSmartRename(true);

    auto addRes = mgr.addWatchedFolder(libDir, /*triggerScan=*/false);
    ASSERT_TRUE(addRes.isOk());

    int progressSignals = 0;
    int libraryUpdatedSignals = 0;
    bool scanDone = false;
    int indexedCount = 0;
    int eventLoopTicks = 0;
    bool allSignalsOnMainThread = true;
    QThread* mainThread = QThread::currentThread();

    QEventLoop loop;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&]() {
        ++eventLoopTicks;
    });

    QObject::connect(
        &mgr, &mavira::app::LibraryManager::scanProgress,
        &loop, [&](int /*processed*/, int /*total*/, const QString&) {
            ++progressSignals;
            if (QThread::currentThread() != mainThread) {
                allSignalsOnMainThread = false;
            }
        });

    QObject::connect(
        &mgr, &mavira::app::LibraryManager::libraryUpdated,
        &loop, [&]() {
            ++libraryUpdatedSignals;
            if (QThread::currentThread() != mainThread) {
                allSignalsOnMainThread = false;
            }
        });

    QObject::connect(
        &mgr, &mavira::app::LibraryManager::scanFinished,
        &loop, [&](int indexed, int /*pruned*/, bool cancelled) {
            EXPECT_FALSE(cancelled);
            indexedCount = indexed;
            scanDone = true;
            loop.quit();
        });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    heartbeat.start();

    // Non-blocking call: returns immediately while WorkerPool scans in the background.
    mgr.scanAllFoldersAsync();
    loop.exec();
    heartbeat.stop();

    ASSERT_TRUE(scanDone) << "Background scan did not finish within timeout";
    EXPECT_TRUE(allSignalsOnMainThread);
    EXPECT_GE(progressSignals, 1);
    EXPECT_GE(libraryUpdatedSignals, 1);
    EXPECT_EQ(indexedCount, 4);

    auto countRes = db_.getTotalComicCount();
    ASSERT_TRUE(countRes.isOk());
    EXPECT_EQ(countRes.value(), 4);

    // Verify ComicInfo.xml metadata was extracted.
    auto vol1Res = db_.getComicByPath(nestedA / L"Vol01.cbz");
    ASSERT_TRUE(vol1Res.isOk());
    ASSERT_TRUE(vol1Res.value().has_value());
    EXPECT_EQ(vol1Res.value()->title, "Neon Horizon Vol 1");
    EXPECT_EQ(vol1Res.value()->series, "Neon Horizon");
    EXPECT_EQ(vol1Res.value()->author, "A. Author");
    EXPECT_EQ(vol1Res.value()->pageCount, 3u);

    // Verify reading progress is preserved across rescans.
    ASSERT_TRUE(db_.updateReadingProgress(vol1Res.value()->id, 2, 1700000000, false).isOk());

    scanDone = false;
    mgr.scanAllFoldersAsync();
    timeout.start(10000);
    loop.exec();
    ASSERT_TRUE(scanDone);

    auto vol1AfterRes = db_.getComicByPath(nestedA / L"Vol01.cbz");
    ASSERT_TRUE(vol1AfterRes.isOk());
    ASSERT_TRUE(vol1AfterRes.value().has_value());
    EXPECT_EQ(vol1AfterRes.value()->currentPage, 2u);

    mgr.shutdown();
}

TEST_F(LibraryManagerTest, DirectoryWatcherRefreshesIncrementallyOnFileAdd) {
    const std::filesystem::path watchDir = tempRoot_ / L"WatchedRoot";
    std::filesystem::create_directories(watchDir / L"Sub");

    mavira::app::LibraryManager mgr(&db_, cache_.get(), portablePaths_);
    mgr.setDebounceIntervalMs(60);

    auto addRes = mgr.addWatchedFolder(watchDir, /*triggerScan=*/false);
    ASSERT_TRUE(addRes.isOk());

    mgr.startWatching();
    EXPECT_TRUE(mgr.isWatching());
    EXPECT_EQ(mgr.activeWatcherCount(), 1u);

    QEventLoop loop;
    bool refreshed = false;
    QObject::connect(&mgr, &mavira::app::LibraryManager::scanFinished, &loop, [&](int indexed, int, bool) {
        if (indexed >= 1) {
            refreshed = true;
            loop.quit();
        }
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);

    // Give the watcher thread a brief moment to arm ReadDirectoryChangesW, then create a new CBZ.
    QTimer::singleShot(100, &loop, [&]() {
        createTestCbz(watchDir / L"Sub" / L"LiveAdded.cbz", 2);
    });

    loop.exec();
    ASSERT_TRUE(refreshed) << "ReadDirectoryChangesW did not trigger incremental refresh";

    auto comicRes = db_.getComicByPath(watchDir / L"Sub" / L"LiveAdded.cbz");
    ASSERT_TRUE(comicRes.isOk());
    ASSERT_TRUE(comicRes.value().has_value());
    EXPECT_EQ(comicRes.value()->pageCount, 2u);

    mgr.stopWatching();
    EXPECT_FALSE(mgr.isWatching());
    EXPECT_EQ(mgr.activeWatcherCount(), 0u);
    mgr.shutdown();
}

TEST_F(LibraryManagerTest, UnpluggedDriveGuardProtectsOfflineRecords) {
    // Pick an unmounted drive letter to simulate an unplugged external USB drive.
    std::filesystem::path offlineRoot;
    for (wchar_t drive = L'Z'; drive >= L'D'; --drive) {
        std::wstring candidate = std::wstring(1, drive) + L":\\MaviraUnpluggedTest";
        if (!mavira::data::fs::FileManager::isVolumeOnline(candidate)) {
            offlineRoot = candidate;
            break;
        }
    }
    if (offlineRoot.empty()) {
        GTEST_SKIP() << "No unmounted drive letter available for unplugged drive test";
    }

    mavira::data::db::ComicRecord existing;
    existing.filePath = offlineRoot / L"Manga" / L"Book1.cbz";
    existing.folderPath = offlineRoot / L"Manga";
    existing.title = "Book 1";
    existing.format = mavira::core::ComicFormat::CBZ;
    existing.pageCount = 24;
    existing.currentPage = 10;
    ASSERT_TRUE(db_.upsertComic(existing).isOk());

    mavira::app::LibraryManager mgr(&db_, cache_.get(), portablePaths_);
    auto addRes = mgr.addWatchedFolder(offlineRoot, /*triggerScan=*/false);
    ASSERT_TRUE(addRes.isOk());

    QEventLoop loop;
    bool scanDone = false;
    bool offlineNotified = false;

    QObject::connect(
        &mgr, &mavira::app::LibraryManager::folderOnlineStateChanged,
        &loop, [&](qint64, const QString&, bool isOnline) {
            if (!isOnline) {
                offlineNotified = true;
            }
        });
    QObject::connect(
        &mgr, &mavira::app::LibraryManager::scanFinished,
        &loop, [&](int indexed, int pruned, bool) {
            EXPECT_EQ(indexed, 0);
            EXPECT_EQ(pruned, 0);
            scanDone = true;
            loop.quit();
        });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);

    mgr.scanAllFoldersAsync();
    loop.exec();

    ASSERT_TRUE(scanDone);
    EXPECT_TRUE(offlineNotified);

    // Record and its reading progress must remain untouched!
    auto checkRes = db_.getComicByPath(existing.filePath);
    ASSERT_TRUE(checkRes.isOk());
    ASSERT_TRUE(checkRes.value().has_value());
    EXPECT_EQ(checkRes.value()->currentPage, 10u);

    mgr.shutdown();
}

TEST_F(LibraryManagerTest, CloudDetectionAndOfflinePinningRoundTrip) {
    // 1. Test cloud sync folder detection using a temporary OneDrive env override.
    const std::filesystem::path fakeOneDrive = tempRoot_ / L"FakeOneDrive";
    std::filesystem::create_directories(fakeOneDrive);

    std::wstring prevOneDrive;
    bool hadPrev = false;
    if (const DWORD len = ::GetEnvironmentVariableW(L"OneDrive", nullptr, 0); len > 0) {
        prevOneDrive.resize(len);
        const DWORD written = ::GetEnvironmentVariableW(L"OneDrive", prevOneDrive.data(), len);
        prevOneDrive.resize(written);
        hadPrev = true;
    }

    ::SetEnvironmentVariableW(L"OneDrive", fakeOneDrive.c_str());
    const auto cloudFolders = mavira::app::LibraryManager::detectCloudSyncFolders();
    const bool inCloud = mavira::app::LibraryManager::isPathInCloudSyncFolder(
        fakeOneDrive / L"Comics" / L"Vol1.cbz");

    // Restore original OneDrive environment variable immediately.
    ::SetEnvironmentVariableW(L"OneDrive", hadPrev ? prevOneDrive.c_str() : nullptr);

    bool foundFakeOneDrive = false;
    for (const auto& cf : cloudFolders) {
        if (cf.path.lexically_normal() == fakeOneDrive.lexically_normal()) {
            foundFakeOneDrive = true;
        }
    }
    EXPECT_TRUE(foundFakeOneDrive);
    EXPECT_TRUE(inCloud);

    // 2. Test background Offline Pinning & Unpinning into <exe_dir>/data/offline_cache/.
    const std::filesystem::path srcCbz = fakeOneDrive / L"Comics" / L"PinnedVol.cbz";
    createTestCbz(srcCbz, 2);

    mavira::data::db::ComicRecord rec;
    rec.filePath = srcCbz;
    rec.folderPath = srcCbz.parent_path();
    rec.title = "PinnedVol";
    rec.format = mavira::core::ComicFormat::CBZ;
    rec.pageCount = 2;
    auto insertRes = db_.upsertComic(rec);
    ASSERT_TRUE(insertRes.isOk());
    const std::int64_t comicId = insertRes.value();

    mavira::app::LibraryManager mgr(&db_, cache_.get(), portablePaths_);

    QEventLoop loop;
    bool pinDone = false;
    bool pinSuccess = false;
    bool pinState = false;
    int progressCount = 0;

    QObject::connect(
        &mgr, &mavira::app::LibraryManager::offlinePinProgress,
        &loop, [&](qint64 id, qint64, qint64) {
            if (id == comicId) {
                ++progressCount;
            }
        });
    QObject::connect(
        &mgr, &mavira::app::LibraryManager::offlinePinFinished,
        &loop, [&](qint64 id, bool pinned, bool success, const QString&) {
            if (id == comicId) {
                pinDone = true;
                pinState = pinned;
                pinSuccess = success;
                loop.quit();
            }
        });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);

    mgr.pinComicOffline(comicId);
    loop.exec();

    ASSERT_TRUE(pinDone);
    EXPECT_TRUE(pinSuccess);
    EXPECT_TRUE(pinState);
    EXPECT_GE(progressCount, 1);

    const std::filesystem::path expectedCachePath =
        mgr.getOfflineCachePath(comicId, srcCbz, mavira::core::ComicFormat::CBZ);
    EXPECT_TRUE(std::filesystem::exists(expectedCachePath));
    EXPECT_EQ(expectedCachePath.parent_path(), portablePaths_.offlineCacheDir);

    auto pinnedRec = db_.getComicById(comicId);
    ASSERT_TRUE(pinnedRec.isOk());
    ASSERT_TRUE(pinnedRec.value().has_value());
    EXPECT_TRUE(pinnedRec.value()->isOfflinePinned);
    EXPECT_EQ(mgr.resolveReadablePath(*pinnedRec.value()), expectedCachePath);

    // Now unpin in the background.
    pinDone = false;
    timeout.start(5000);
    mgr.unpinComicOffline(comicId);
    loop.exec();

    ASSERT_TRUE(pinDone);
    EXPECT_TRUE(pinSuccess);
    EXPECT_FALSE(pinState);
    EXPECT_FALSE(std::filesystem::exists(expectedCachePath));

    auto unpinnedRec = db_.getComicById(comicId);
    ASSERT_TRUE(unpinnedRec.isOk());
    ASSERT_TRUE(unpinnedRec.value().has_value());
    EXPECT_FALSE(unpinnedRec.value()->isOfflinePinned);
    EXPECT_EQ(mgr.resolveReadablePath(*unpinnedRec.value()), srcCbz);

    mgr.shutdown();
}

TEST_F(LibraryManagerTest, RemoveComicFromLibraryAndOrphanPruneRespectsUnpluggedDriveGuard) {
    const std::filesystem::path standaloneCbz = tempRoot_ / L"Standalone" / L"Removed.cbz";
    createTestCbz(standaloneCbz, 2);

    mavira::data::db::ComicRecord rec;
    rec.filePath = standaloneCbz;
    rec.folderPath = standaloneCbz.parent_path();
    rec.title = "Removed Comic";
    rec.format = mavira::core::ComicFormat::CBZ;
    rec.pageCount = 2;
    auto upRes = db_.upsertComic(rec);
    ASSERT_TRUE(upRes.isOk());
    const std::int64_t comicId = upRes.value();

    // Add a fake record on an offline/disconnected drive letter (Z:\) to verify Unplugged Drive Guard.
    mavira::data::db::ComicRecord offlineRec;
    offlineRec.filePath = std::filesystem::path(L"Z:\\DisconnectedDrive\\OfflineComic.cbz");
    offlineRec.folderPath = std::filesystem::path(L"Z:\\DisconnectedDrive");
    offlineRec.title = "Offline Drive Comic";
    offlineRec.format = mavira::core::ComicFormat::CBZ;
    offlineRec.pageCount = 5;
    auto offlineRes = db_.upsertComic(offlineRec);
    ASSERT_TRUE(offlineRes.isOk());
    const std::int64_t offlineId = offlineRes.value();

    mavira::app::LibraryManager mgr(&db_, cache_.get(), portablePaths_);

    int updatedSignals = 0;
    int removedSignals = 0;
    std::int64_t lastRemovedId = 0;
    std::wstring lastRemovedPath;
    QObject::connect(&mgr, &mavira::app::LibraryManager::libraryUpdated, &mgr, [&]() {
        ++updatedSignals;
    });
    QObject::connect(
        &mgr, &mavira::app::LibraryManager::comicRemoved,
        &mgr, [&](std::int64_t id, const std::wstring& path) {
            ++removedSignals;
            lastRemovedId = id;
            lastRemovedPath = path;
        });

    // Remove from library without deleting from disk -> DB record gone, file remains, signal emitted.
    auto remRes = mgr.removeComicFromLibrary(comicId, /*deleteFromDisk=*/false);
    ASSERT_TRUE(remRes.isOk()) << remRes.error().message;
    EXPECT_EQ(updatedSignals, 1);
    EXPECT_EQ(removedSignals, 1);
    EXPECT_EQ(lastRemovedId, comicId);
    EXPECT_EQ(lastRemovedPath, standaloneCbz.native());
    EXPECT_TRUE(std::filesystem::exists(standaloneCbz));
    auto afterRem = db_.getComicById(comicId);
    ASSERT_TRUE(afterRem.isOk());
    EXPECT_FALSE(afterRem.value().has_value());

    // Add another standalone record, delete its file from disk, and run pruneAllOrphans.
    const std::filesystem::path missingCbz = tempRoot_ / L"Standalone" / L"DeletedExternally.cbz";
    createTestCbz(missingCbz, 1);
    mavira::data::db::ComicRecord rec2;
    rec2.filePath = missingCbz;
    rec2.folderPath = missingCbz.parent_path();
    rec2.title = "Deleted Externally";
    rec2.format = mavira::core::ComicFormat::CBZ;
    rec2.pageCount = 1;
    auto upRes2 = db_.upsertComic(rec2);
    ASSERT_TRUE(upRes2.isOk());
    const std::int64_t missingId = upRes2.value();

    std::error_code ec;
    ASSERT_TRUE(std::filesystem::remove(missingCbz, ec));

    auto pruneRes = db_.pruneAllOrphans(
        [](const std::filesystem::path& p) {
            return mavira::data::fs::FileManager::isVolumeOnline(p);
        },
        [](const std::filesystem::path& p) {
            return mavira::data::fs::FileManager::isPathOnline(p);
        });
    ASSERT_TRUE(pruneRes.isOk()) << pruneRes.error().message;
    EXPECT_EQ(pruneRes.value().pruned, 1u);
    EXPECT_GE(pruneRes.value().skippedOfflineRoot, 1u);

    auto missingAfter = db_.getComicById(missingId);
    ASSERT_TRUE(missingAfter.isOk());
    EXPECT_FALSE(missingAfter.value().has_value());

    // Offline drive comic must STILL be present (Unplugged Drive Guard).
    auto offlineAfter = db_.getComicById(offlineId);
    ASSERT_TRUE(offlineAfter.isOk());
    EXPECT_TRUE(offlineAfter.value().has_value());

    mgr.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
