#include "app/ApplicationCore.hpp"
#include "app/LibraryManager.hpp"
#include "core/cache/CacheManager.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/MainWindow.hpp"

#include <Windows.h>
#include <gtest/gtest.h>
#include <zip.h>

#include <QApplication>
#include <QFileInfo>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using mavira::app::ApplicationCore;
using mavira::app::JumpListEntry;
using mavira::app::LibraryManager;
using mavira::core::ImageFrame;
using mavira::core::cache::CacheManager;
using mavira::data::config::AppSettings;
using mavira::data::config::SettingsManager;
using mavira::data::db::ComicRecord;
using mavira::data::db::DatabaseManager;
using mavira::data::fs::FileManager;
using mavira::data::fs::PortablePaths;
using mavira::ui::ActiveViewMode;
using mavira::ui::MainWindow;

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

void createMinimalCbz(const std::filesystem::path& cbzPath) {
    std::filesystem::create_directories(cbzPath.parent_path());

    ImageFrame frame;
    frame.width = 64;
    frame.height = 96;
    frame.stride = 64 * 4;
    frame.pixels.assign(static_cast<std::size_t>(frame.stride) * frame.height, 0xCC);
    auto jpgRes = mavira::core::imaging::ImageDecoder::encodeJpeg(frame, 85);
    ASSERT_TRUE(jpgRes.isOk());
    const auto& jpg = jpgRes.value();

    const std::wstring wide = FileManager::toWin32LongPath(cbzPath);
    zip_error_t err;
    ::zip_error_init(&err);
    zip_source_t* src = ::zip_source_win32w_create(wide.c_str(), 0, -1, &err);
    ASSERT_NE(src, nullptr);
    zip_t* za = ::zip_open_from_source(src, ZIP_CREATE | ZIP_TRUNCATE, &err);
    ASSERT_NE(za, nullptr);
    ::zip_error_fini(&err);

    zip_source_t* buf = ::zip_source_buffer(za, jpg.data(), jpg.size(), 0);
    ASSERT_NE(buf, nullptr);
    ASSERT_GE(::zip_file_add(za, "001.jpg", buf, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE), 0);
    ASSERT_EQ(::zip_close(za), 0);
}

TEST(ApplicationCoreTest, KeepScreenActiveTogglesWin32ExecutionStateWithReaderView) {
    TempDirGuard tmp(L"mavira_test_keep_screen_active");

    const PortablePaths paths = FileManager::resolvePortablePaths(tmp.path);
    ASSERT_TRUE(FileManager::ensurePortableDirectories(paths).isOk());

    SettingsManager settingsMgr(paths.settingsPath);
    DatabaseManager db;
    ASSERT_TRUE(db.open(paths.databasePath).isOk());
    CacheManager cache;
    LibraryManager libMgr(&db, &cache, paths);
    MainWindow win(&db, &cache, &libMgr, &settingsMgr);

    ASSERT_NE(win.applicationCore(), nullptr);
    EXPECT_FALSE(win.applicationCore()->isKeepScreenActive());

    // Switching to Reader view with keep_screen_active=true enables ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED
    win.switchToView(ActiveViewMode::Reader);
    EXPECT_TRUE(win.applicationCore()->isKeepScreenActive());
    const unsigned long activeFlags = win.applicationCore()->lastExecutionStateFlags();
    EXPECT_NE(activeFlags & ES_CONTINUOUS, 0u);
    EXPECT_NE(activeFlags & ES_DISPLAY_REQUIRED, 0u);
    EXPECT_NE(activeFlags & ES_SYSTEM_REQUIRED, 0u);

    // Switching back to Library resets execution state to ES_CONTINUOUS
    win.switchToView(ActiveViewMode::Library);
    EXPECT_FALSE(win.applicationCore()->isKeepScreenActive());
    EXPECT_EQ(win.applicationCore()->lastExecutionStateFlags(), static_cast<unsigned long>(ES_CONTINUOUS));

    // Disabling keep_screen_active in settings prevents activation in Reader view
    AppSettings updated = win.currentSettings();
    updated.keep_screen_active = false;
    win.applySettings(updated, true);
    win.switchToView(ActiveViewMode::Reader);
    EXPECT_FALSE(win.applicationCore()->isKeepScreenActive());

    libMgr.shutdown();
    db.close();
}

TEST(ApplicationCoreTest, TaskbarJumpListUpdatesFromRecentComics) {
    TempDirGuard tmp(L"mavira_test_jumplist");
    const auto cbz1 = tmp.path / L"Volume_01.cbz";
    const auto cbz2 = tmp.path / L"Volume_02.cbz";
    createMinimalCbz(cbz1);
    createMinimalCbz(cbz2);

    DatabaseManager db;
    ASSERT_TRUE(db.open(tmp.path / L"jumplist.db").isOk());

    ComicRecord rec1;
    rec1.filePath = cbz1;
    rec1.folderPath = tmp.path;
    rec1.title = "Jump List Volume 1";
    rec1.currentPage = 1;
    rec1.pageCount = 10;
    rec1.lastReadUnix = 2000;
    ASSERT_TRUE(db.upsertComic(rec1).isOk());

    ComicRecord rec2;
    rec2.filePath = cbz2;
    rec2.folderPath = tmp.path;
    rec2.title = "Jump List Volume 2";
    rec2.currentPage = 2;
    rec2.pageCount = 10;
    rec2.lastReadUnix = 3000;
    ASSERT_TRUE(db.upsertComic(rec2).isOk());

    ApplicationCore::ensureAppUserModelId();
    ApplicationCore appCore(&db);
    auto res = appCore.refreshJumpListFromDatabase(10, false);
    ASSERT_TRUE(res.isOk()) << res.error().message;
    EXPECT_EQ(res.value(), 2u);
    EXPECT_EQ(appCore.lastJumpListCount(), 2u);

    // Clear Jump List entries cleanly after test
    (void)appCore.updateJumpList({});
    db.close();
}

TEST(ApplicationCoreTest, ExplorerCliOpenAndExternalAppHandoff) {
    TempDirGuard tmp(L"mavira_test_cli_and_handoff");
    const auto cbzPath = tmp.path / L"Explorer_Double_Click.cbz";
    createMinimalCbz(cbzPath);

    // 1. Verify CLI argument parsing from Explorer double-click
    const QStringList cliArgs{
        QStringLiteral("C:\\Program Files\\MAVIRA\\MAVIRA.exe"),
        QString::fromStdWString(cbzPath.native())
    };
    const auto parsed = ApplicationCore::extractLaunchFilePath(cliArgs);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, cbzPath);

    // Verify opening the parsed path directly in MainWindow switches to Reader view
    {
        const PortablePaths paths = FileManager::resolvePortablePaths(tmp.path);
        ASSERT_TRUE(FileManager::ensurePortableDirectories(paths).isOk());

        SettingsManager settingsMgr(paths.settingsPath);
        DatabaseManager db;
        ASSERT_TRUE(db.open(paths.databasePath).isOk());
        CacheManager cache;
        LibraryManager libMgr(&db, &cache, paths);
        MainWindow win(&db, &cache, &libMgr, &settingsMgr);

        win.openComicPath(*parsed);
        EXPECT_EQ(win.activeViewMode(), ActiveViewMode::Reader);
        ASSERT_NE(win.readerController(), nullptr);
        EXPECT_TRUE(win.readerController()->isOpen());
        EXPECT_EQ(win.readerController()->currentPath(), cbzPath);

        libMgr.shutdown();
        db.close();
    }

    // 2. Verify External App Handoff via QProcess::startDetached
    const auto markerOut = tmp.path / L"external_app_received.txt";
    const auto batchLauncher = tmp.path / L"mock_vellum.cmd";
    {
        std::ofstream ofs(batchLauncher, std::ios::trunc);
        ofs << "@echo off\n";
        ofs << "echo %~1>\"" << FileManager::wideToUtf8(markerOut.native()) << "\"\n";
    }

    auto handoffRes = ApplicationCore::launchExternalApp(cbzPath, batchLauncher);
    ASSERT_TRUE(handoffRes.isOk()) << handoffRes.error().message;
    EXPECT_GT(handoffRes.value(), 0);

    bool received = false;
    for (int i = 0; i < 40; ++i) {
        if (std::filesystem::exists(markerOut) && std::filesystem::file_size(markerOut) > 0) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(received) << "External companion app did not write handoff marker file";

    std::ifstream ifs(markerOut);
    std::string line;
    std::getline(ifs, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    EXPECT_EQ(std::filesystem::path(FileManager::utf8ToWide(line)), cbzPath);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
