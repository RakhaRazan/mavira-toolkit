#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using mavira::core::ComicFormat;
using mavira::data::config::AppSettings;
using mavira::data::config::SettingsManager;
using mavira::data::db::ComicRecord;
using mavira::data::db::DatabaseManager;
using mavira::data::fs::FileManager;

struct TempDbDirGuard {
    std::filesystem::path dir;
    std::filesystem::path dbPath;

    explicit TempDbDirGuard(const std::wstring& name) {
        dir = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        dbPath = dir / L"library.db";
    }

    ~TempDbDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

std::wstring toLowerKey(const std::filesystem::path& p) {
    std::wstring s = FileManager::stripWin32LongPathPrefix(p.lexically_normal().native());
    for (auto& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return s;
}

TEST(DatabaseFtsTest, Fts5SearchUnder5MsAcross10000PlusRecords) {
    TempDbDirGuard tmp(L"mavira_test_db_fts10k");
    DatabaseManager db;
    ASSERT_TRUE(db.open(tmp.dbPath).isOk());

    constexpr std::size_t kRecordCount = 10500;
    std::vector<ComicRecord> batch;
    batch.reserve(kRecordCount);

    for (std::size_t i = 0; i < kRecordCount; ++i) {
        ComicRecord rec;
        const std::size_t seriesIdx = i / 50;
        const std::size_t volIdx = (i % 50) + 1;
        rec.folderPath = std::filesystem::path(L"D:\\Library\\Series_" + std::to_wstring(seriesIdx));
        rec.filePath = rec.folderPath / (L"Vol_" + std::to_wstring(volIdx) + L".cbz");
        rec.title = "Chronicle Episode " + std::to_string(i);
        rec.series = "Series_" + std::to_string(seriesIdx);
        rec.author = "Author_" + std::to_string(seriesIdx % 40);
        rec.tags = (i % 3 == 0) ? "action,fantasy,seinen" : "slice_of_life,comedy";
        rec.format = ComicFormat::CBZ;
        rec.pageCount = 180;
        rec.fileSizeBytes = 64 * 1024 * 1024;
        batch.push_back(std::move(rec));
    }

    // Plant a few distinct needle records for FTS5 search verification.
    batch[4242].title = "Berserk Deluxe Edition Vol 14";
    batch[4242].series = "Berserk";
    batch[4242].author = "Kentaro Miura";
    batch[4242].tags = "dark_fantasy,seinen,classic";

    batch[8888].title = "Berserk Official Guidebook";
    batch[8888].series = "Berserk";
    batch[8888].author = "Kentaro Miura";
    batch[8888].tags = "artbook,guide,dark_fantasy";

    auto insertRes = db.upsertComicsBatched(batch, 500);
    ASSERT_TRUE(insertRes.isOk());
    EXPECT_EQ(insertRes.value(), kRecordCount);

    auto countRes = db.getTotalComicCount();
    ASSERT_TRUE(countRes.isOk());
    EXPECT_EQ(countRes.value(), static_cast<std::int64_t>(kRecordCount));

    // Warm up SQLite page cache once, then measure FTS5 search latency.
    auto warmRes = db.searchComics("Berserk Miura", 50);
    ASSERT_TRUE(warmRes.isOk());
    ASSERT_EQ(warmRes.value().size(), 2u);

    const auto t0 = std::chrono::steady_clock::now();
    auto searchRes = db.searchComics("Berserk Miura", 50);
    const auto t1 = std::chrono::steady_clock::now();

    ASSERT_TRUE(searchRes.isOk());
    ASSERT_EQ(searchRes.value().size(), 2u);

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "[Benchmark] FTS5 search across " << kRecordCount
              << " records took " << elapsedMs << " ms\n";
    EXPECT_LT(elapsedMs, 5.0);

    // Verify FTS5 triggers stay synced on UPDATE and DELETE.
    const std::int64_t targetId = searchRes.value().front().id;
    auto fetched = db.getComicById(targetId);
    ASSERT_TRUE(fetched.isOk() && fetched.value().has_value());
    ComicRecord updated = *fetched.value();
    updated.title = "Vagabond Vizbig Vol 1";
    updated.series = "Vagabond";
    updated.author = "Takehiko Inoue";
    ASSERT_TRUE(db.upsertComic(updated).isOk());

    auto afterUpdate = db.searchComics("Vagabond Inoue", 10);
    ASSERT_TRUE(afterUpdate.isOk());
    ASSERT_EQ(afterUpdate.value().size(), 1u);
    EXPECT_EQ(afterUpdate.value().front().id, targetId);

    ASSERT_TRUE(db.deleteComic(targetId).isOk());
    auto afterDelete = db.searchComics("Vagabond Inoue", 10);
    ASSERT_TRUE(afterDelete.isOk());
    EXPECT_TRUE(afterDelete.value().empty());
}

TEST(DatabaseFtsTest, UnpluggedDriveGuardProtectsOfflineRootsAndPinnedComics) {
    TempDbDirGuard tmp(L"mavira_test_db_guard");
    DatabaseManager db;
    ASSERT_TRUE(db.open(tmp.dbPath).isOk());

    const std::filesystem::path externalRoot = L"Z:\\ExternalDrive\\Manga";
    ASSERT_TRUE(db.addWatchedFolder(externalRoot, "\\\\?\\Volume{TEST-GUID}\\").isOk());

    ComicRecord normalComic;
    normalComic.folderPath = externalRoot / L"OnePiece";
    normalComic.filePath = normalComic.folderPath / L"Vol1.cbz";
    normalComic.title = "One Piece Vol 1";
    normalComic.isOfflinePinned = false;

    ComicRecord pinnedComic;
    pinnedComic.folderPath = externalRoot / L"OnePiece";
    pinnedComic.filePath = pinnedComic.folderPath / L"Vol2.cbz";
    pinnedComic.title = "One Piece Vol 2";
    pinnedComic.isOfflinePinned = true;

    ComicRecord presentComic;
    presentComic.folderPath = externalRoot / L"OnePiece";
    presentComic.filePath = presentComic.folderPath / L"Vol3.cbz";
    presentComic.title = "One Piece Vol 3";
    presentComic.isOfflinePinned = false;

    ASSERT_TRUE(db.upsertComicsBatched({normalComic, pinnedComic, presentComic}, 500).isOk());

    // 1. Root drive is OFFLINE (default FileManager::isPathOnline on Z:\ExternalDrive\Manga returns false):
    //    Unplugged Drive Guard must prune ZERO records even if discovered set is empty!
    std::unordered_set<std::wstring> emptyDiscovered;
    auto offlineReport = db.pruneOrphans(externalRoot, emptyDiscovered);
    ASSERT_TRUE(offlineReport.isOk());
    EXPECT_EQ(offlineReport.value().pruned, 0u);
    EXPECT_EQ(offlineReport.value().skippedOfflineRoot, 3u);
    EXPECT_EQ(db.getTotalComicCount().value(), 3);

    // 2. Root drive is ONLINE, Vol3 exists, Vol1 & Vol2 are missing on disk,
    //    but Vol2 has is_offline_pinned == 1 -> ONLY Vol1 may be pruned!
    std::unordered_set<std::wstring> discoveredOnlyVol3 = {toLowerKey(presentComic.filePath)};
    auto onlineReport = db.pruneOrphans(
        externalRoot,
        discoveredOnlyVol3,
        [](const std::filesystem::path&) { return true; });
    ASSERT_TRUE(onlineReport.isOk());
    EXPECT_EQ(onlineReport.value().scanned, 3u);
    EXPECT_EQ(onlineReport.value().pruned, 1u);
    EXPECT_EQ(onlineReport.value().skippedOfflinePinned, 1u);

    EXPECT_FALSE(db.getComicByPath(normalComic.filePath).value().has_value());
    EXPECT_TRUE(db.getComicByPath(pinnedComic.filePath).value().has_value());
    EXPECT_TRUE(db.getComicByPath(presentComic.filePath).value().has_value());
}

TEST(DatabaseFtsTest, FolderHierarchyContinueReadingRandomUnreadAndCollections) {
    TempDbDirGuard tmp(L"mavira_test_db_queries");
    DatabaseManager db;
    ASSERT_TRUE(db.open(tmp.dbPath).isOk());

    const std::filesystem::path root = L"E:\\Comics";

    std::vector<ComicRecord> comics;
    auto makeComic = [&](const std::wstring& relFolder, const std::wstring& file, std::string title) {
        ComicRecord r;
        r.folderPath = root / relFolder;
        r.filePath = r.folderPath / file;
        r.title = std::move(title);
        r.format = ComicFormat::CBZ;
        r.pageCount = 100;
        return r;
    };

    comics.push_back(makeComic(L"Series10", L"Vol1.cbz", "Series 10 Vol 1"));
    comics.push_back(makeComic(L"Series2", L"Vol1.cbz", "Series 2 Vol 1"));
    comics.push_back(makeComic(L"Series2\\Arc1", L"Vol2.cbz", "Series 2 Arc 1"));
    comics.push_back(makeComic(L"Series1", L"Vol1.cbz", "Series 1 Vol 1"));

    ASSERT_TRUE(db.upsertComicsBatched(comics, 500).isOk());

    // Indexed child folder query returns immediate children in natural order
    auto childrenRes = db.getChildFolders(root);
    ASSERT_TRUE(childrenRes.isOk());
    ASSERT_EQ(childrenRes.value().size(), 3u);
    EXPECT_EQ(childrenRes.value()[0].filename(), L"Series1");
    EXPECT_EQ(childrenRes.value()[1].filename(), L"Series2");
    EXPECT_EQ(childrenRes.value()[2].filename(), L"Series10");

    // Continue Reading & Random Unread
    auto c1 = db.getComicByPath(comics[0].filePath).value();
    auto c2 = db.getComicByPath(comics[1].filePath).value();
    ASSERT_TRUE(c1.has_value() && c2.has_value());

    ASSERT_TRUE(db.updateReadingProgress(c1->id, 25, 1700000100, false).isOk());
    ASSERT_TRUE(db.updateReadingProgress(c2->id, 40, 1700000200, false).isOk());

    auto contRes = db.getContinueReading(10);
    ASSERT_TRUE(contRes.isOk());
    ASSERT_EQ(contRes.value().size(), 2u);
    EXPECT_EQ(contRes.value()[0].id, c2->id); // Most recently read first
    EXPECT_EQ(contRes.value()[1].id, c1->id);

    auto rndRes = db.getRandomUnread();
    ASSERT_TRUE(rndRes.isOk() && rndRes.value().has_value());
    EXPECT_EQ(rndRes.value()->currentPage, 0u);
    EXPECT_FALSE(rndRes.value()->isRead);

    // Bookmarks, Shelves, SmartViews
    auto bmId = db.addBookmark(c1->id, 42, "Epic Battle", 1700000300);
    ASSERT_TRUE(bmId.isOk());
    auto bms = db.getBookmarksForComic(c1->id);
    ASSERT_TRUE(bms.isOk() && bms.value().size() == 1u);
    EXPECT_EQ(bms.value()[0].name, "Epic Battle");

    auto shelfId = db.createShelf("Favorites", 1, 1700000300);
    ASSERT_TRUE(shelfId.isOk());
    ASSERT_TRUE(db.addComicToShelf(shelfId.value(), c1->id, 0, 1700000300).isOk());
    auto shelfItems = db.getComicsInShelf(shelfId.value());
    ASSERT_TRUE(shelfItems.isOk() && shelfItems.value().size() == 1u);
    EXPECT_EQ(shelfItems.value()[0].id, c1->id);

    auto svId = db.createSmartView("Unread Seinen", R"({"tag":"seinen","unread":true})", 1);
    ASSERT_TRUE(svId.isOk());
    auto svs = db.getSmartViews();
    ASSERT_TRUE(svs.isOk() && svs.value().size() == 1u);
    EXPECT_EQ(svs.value()[0].name, "Unread Seinen");
}

TEST(FileManagerTest, NaturalCompareOrdersNumbersNaturally) {
    EXPECT_LT(FileManager::naturalCompare(L"page2.jpg", L"page10.jpg"), 0);
    EXPECT_GT(FileManager::naturalCompare(L"page10.jpg", L"page2.jpg"), 0);
    EXPECT_EQ(FileManager::naturalCompare(L"Page10.JPG", L"page10.jpg"), 0);

    std::vector<std::wstring> pages = {
        L"page10.jpg",
        L"page2.jpg",
        L"page1.jpg",
        L"page20.jpg",
        L"page3.jpg",
        L"page100.jpg"
    };

    std::sort(pages.begin(), pages.end(), [](const std::wstring& a, const std::wstring& b) {
        return FileManager::naturalLess(a, b);
    });

    const std::vector<std::wstring> expected = {
        L"page1.jpg",
        L"page2.jpg",
        L"page3.jpg",
        L"page10.jpg",
        L"page20.jpg",
        L"page100.jpg"
    };
    EXPECT_EQ(pages, expected);

    // Also verify narrow string_view overload and huge digit runs without overflow
    EXPECT_LT(FileManager::naturalCompare("vol9_ch2", "vol9_ch10"), 0);
    EXPECT_LT(FileManager::naturalCompare("id99999999999999999999", "id100000000000000000000"), 0);
}

TEST(FileManagerTest, PortablePathsAndLongPathHelpers) {
    TempDbDirGuard tmp(L"mavira_test_portable");
    const auto paths = FileManager::resolvePortablePaths(tmp.dir);

    EXPECT_EQ(paths.dataDir, tmp.dir / L"data");
    EXPECT_EQ(paths.databasePath, tmp.dir / L"data" / L"library.db");
    EXPECT_EQ(paths.coversPackPath, tmp.dir / L"data" / L"covers.pack");
    EXPECT_EQ(paths.offlineCacheDir, tmp.dir / L"data" / L"offline_cache");
    EXPECT_EQ(paths.settingsPath, tmp.dir / L"data" / L"settings.json");
    EXPECT_EQ(paths.logsDir, tmp.dir / L"data" / L"logs");

    // Master-folder runtime/ layout resolves data/ to parent folder
    const auto runtimePaths = FileManager::resolvePortablePaths(tmp.dir / L"runtime");
    EXPECT_EQ(runtimePaths.dataDir, tmp.dir / L"data");
    EXPECT_EQ(runtimePaths.databasePath, tmp.dir / L"data" / L"library.db");
    EXPECT_EQ(runtimePaths.coversPackPath, tmp.dir / L"data" / L"covers.pack");
    EXPECT_EQ(runtimePaths.settingsPath, tmp.dir / L"data" / L"settings.json");

    auto ensureRes = FileManager::ensurePortableDirectories(paths);
    ASSERT_TRUE(ensureRes.isOk());
    EXPECT_TRUE(FileManager::isPathOnline(paths.dataDir));
    EXPECT_TRUE(FileManager::isPathOnline(paths.offlineCacheDir));
    EXPECT_TRUE(FileManager::isPathOnline(paths.logsDir));

    const std::wstring longPath = FileManager::toWin32LongPath(paths.settingsPath);
    EXPECT_TRUE(longPath.starts_with(L"\\\\?\\"));
    EXPECT_EQ(FileManager::stripWin32LongPathPrefix(longPath), paths.settingsPath.native());

    const std::wstring uncLong = FileManager::toWin32LongPath(L"\\\\fileserver\\manga\\vol1.cbz");
    EXPECT_EQ(uncLong, L"\\\\?\\UNC\\fileserver\\manga\\vol1.cbz");
    EXPECT_EQ(FileManager::stripWin32LongPathPrefix(uncLong), L"\\\\fileserver\\manga\\vol1.cbz");

    EXPECT_FALSE(FileManager::isPathOnline(L"Z:\\nonexistent_mavira_drive_12345\\comic.cbz"));
}

TEST(FileManagerTest, ScanComicsInFolderDiscoversArchivesAndImageFolders) {
    TempDbDirGuard tmp(L"mavira_test_scan");

    auto touchFile = [](const std::filesystem::path& p, const std::string& content = "data") {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream ofs(p, std::ios::binary);
        ofs << content;
    };

    touchFile(tmp.dir / L"Vol10.cbz");
    touchFile(tmp.dir / L"Vol2.cbr");
    touchFile(tmp.dir / L"Vol1.pdf");
    touchFile(tmp.dir / L"Novel.epub");
    touchFile(tmp.dir / L"Pack.cb7");
    touchFile(tmp.dir / L"Tarball.cbt");
    touchFile(tmp.dir / L"Notes.txt");
    touchFile(tmp.dir / L"RawChapter" / L"page1.jpg", "img1");
    touchFile(tmp.dir / L"RawChapter" / L"page2.png", "img2");

    auto scanRes = FileManager::scanComicsInFolder(tmp.dir);
    ASSERT_TRUE(scanRes.isOk());

    const auto& comics = scanRes.value();
    ASSERT_EQ(comics.size(), 7u);

    auto findByFilename = [&](const std::wstring& name) {
        return std::find_if(comics.begin(), comics.end(), [&](const auto& c) {
            return c.path.filename().native() == name;
        });
    };

    auto itVol1 = findByFilename(L"Vol1.pdf");
    auto itVol2 = findByFilename(L"Vol2.cbr");
    auto itVol10 = findByFilename(L"Vol10.cbz");
    auto itRaw = findByFilename(L"RawChapter");
    auto itEpub = findByFilename(L"Novel.epub");

    ASSERT_NE(itVol1, comics.end());
    ASSERT_NE(itVol2, comics.end());
    ASSERT_NE(itVol10, comics.end());
    ASSERT_NE(itRaw, comics.end());
    ASSERT_NE(itEpub, comics.end());

    EXPECT_EQ(itVol1->format, ComicFormat::PDF);
    EXPECT_EQ(itVol2->format, ComicFormat::CBR);
    EXPECT_EQ(itVol10->format, ComicFormat::CBZ);
    EXPECT_EQ(itRaw->format, ComicFormat::Folder);
    EXPECT_EQ(itRaw->fileSizeBytes, 8u);
    EXPECT_EQ(itEpub->format, ComicFormat::EPUB);

    // Natural sort order check: Vol2.cbr comes before Vol10.cbz
    EXPECT_LT(std::distance(comics.begin(), itVol2), std::distance(comics.begin(), itVol10));
}

TEST(SettingsManagerTest, AtomicSaveAndLoadRoundTrip) {
    TempDbDirGuard tmp(L"mavira_test_settings");
    const std::filesystem::path settingsFile = tmp.dir / L"data" / L"settings.json";
    SettingsManager manager(settingsFile);

    // Missing file returns defaults
    auto initialRes = manager.load();
    ASSERT_TRUE(initialRes.isOk());
    const AppSettings defaults = initialRes.value();
    EXPECT_EQ(defaults.cover_cache_mode, 1);
    EXPECT_EQ(defaults.ram_cache_limit_mb, 128);
    EXPECT_TRUE(defaults.default_manga_mode);
    EXPECT_TRUE(defaults.smart_wide_spread);
    EXPECT_FALSE(defaults.auto_crop_borders);
    EXPECT_FALSE(defaults.vertical_webtoon_mode);
    EXPECT_TRUE(defaults.keep_screen_active);
    EXPECT_TRUE(defaults.circular_progress_bar);
    EXPECT_FALSE(defaults.smart_rename);
    EXPECT_FALSE(defaults.show_hidden_items);
    EXPECT_TRUE(defaults.loupe_magnifier_enabled);
    EXPECT_EQ(defaults.theme, "obsidian_cyber");
    EXPECT_TRUE(SettingsManager::isValidThemePreset("obsidian_cyber"));
    EXPECT_TRUE(SettingsManager::isValidThemePreset("midnight_velvet"));
    EXPECT_TRUE(SettingsManager::isValidThemePreset("warm_parchment"));
    EXPECT_TRUE(SettingsManager::isValidThemePreset("nordic_glacier"));
    EXPECT_TRUE(SettingsManager::isValidThemePreset("sakura_mist"));
    EXPECT_FALSE(SettingsManager::isValidThemePreset("dark"));
    EXPECT_TRUE(SettingsManager::isValidLanguage("en"));
    EXPECT_TRUE(SettingsManager::isValidLanguage("id"));
    EXPECT_TRUE(SettingsManager::isValidLanguage("zh"));
    EXPECT_TRUE(SettingsManager::isValidLanguage("ja"));
    EXPECT_TRUE(SettingsManager::isValidLanguage("ko"));
    EXPECT_FALSE(SettingsManager::isValidLanguage("fr"));
    EXPECT_TRUE(SettingsManager::isValidCardDensity("compact"));
    EXPECT_TRUE(SettingsManager::isValidCardDensity("comfortable"));
    EXPECT_TRUE(SettingsManager::isValidCardDensity("spacious"));
    EXPECT_FALSE(SettingsManager::isValidCardDensity("tiny"));
    EXPECT_EQ(SettingsManager::clampUiScalePercent(70), 90);
    EXPECT_EQ(SettingsManager::clampUiScalePercent(150), 125);
    EXPECT_EQ(SettingsManager::clampUiScalePercent(110), 110);
    EXPECT_EQ(defaults.language, "en");
    EXPECT_EQ(defaults.ui_scale_percent, 100);
    EXPECT_EQ(defaults.card_density, "comfortable");
    EXPECT_TRUE(defaults.confirm_delete_disk);
    EXPECT_EQ(defaults.cursor_hide_seconds, 2);
    EXPECT_TRUE(defaults.show_status_telemetry);
    EXPECT_EQ(defaults.default_page_layout, "single");
    EXPECT_EQ(defaults.default_fit_mode, "fit_width");
    EXPECT_EQ(defaults.reader_bg_color, "#0B0D10");
    EXPECT_DOUBLE_EQ(defaults.loupe_zoom_factor, 3.0);
    EXPECT_EQ(defaults.loupe_size_px, 220);
    EXPECT_EQ(defaults.pdf_reader_dpi, 200);
    EXPECT_EQ(defaults.default_export_format, "cbz");
    EXPECT_EQ(defaults.external_app_args, "\"{file}\"");
    EXPECT_EQ(defaults.log_level, "warn");
    EXPECT_FALSE(defaults.shortcuts.empty());

    // Modify settings and save atomically
    AppSettings custom = defaults;
    custom.cover_cache_mode = 2;
    custom.ram_cache_limit_mb = 256;
    custom.default_manga_mode = false;
    custom.auto_crop_borders = true;
    custom.vertical_webtoon_mode = true;
    custom.theme = "amoled";
    custom.language = "ja";
    custom.ui_scale_percent = 125;
    custom.card_density = "compact";
    custom.confirm_delete_disk = false;
    custom.cursor_hide_seconds = 5;
    custom.show_status_telemetry = false;
    custom.default_page_layout = "double";
    custom.default_fit_mode = "fit_height";
    custom.reader_bg_color = "#111111";
    custom.loupe_zoom_factor = 4.5;
    custom.loupe_size_px = 280;
    custom.pdf_reader_dpi = 300;
    custom.default_export_format = "pdf";
    custom.external_app_path = "C:\\Tools\\Viewer.exe";
    custom.external_app_args = "--open \"{file}\"";
    custom.log_level = "debug";
    custom.shortcuts["next_page"] = "Space";
    custom.shortcuts["custom_action"] = "Ctrl+Shift+\"K\"";

    auto saveRes = manager.save(custom);
    ASSERT_TRUE(saveRes.isOk());

    // Temporary file must not remain after atomic MoveFileExW
    std::filesystem::path tmpFile = settingsFile;
    tmpFile += L".tmp";
    EXPECT_TRUE(std::filesystem::exists(settingsFile));
    EXPECT_FALSE(std::filesystem::exists(tmpFile));

    // Reload and verify exact equality
    auto reloadRes = manager.load();
    ASSERT_TRUE(reloadRes.isOk());
    EXPECT_EQ(reloadRes.value(), custom);

    // Overwrite atomically a second time to confirm MOVEFILE_REPLACE_EXISTING works
    custom.theme = "light";
    ASSERT_TRUE(manager.save(custom).isOk());
    auto reloadRes2 = manager.load();
    ASSERT_TRUE(reloadRes2.isOk());
    EXPECT_EQ(reloadRes2.value().theme, "light");

    // Verify invalid language/density/scale values sanitize cleanly on deserialize
    const std::string dirtyJson =
        R"({"language":"fr","ui_scale_percent":160,"card_density":"huge","manga_enhancer_enabled":true,"auto_scroll_speed":9})";
    auto sanitizedRes = SettingsManager::deserialize(dirtyJson);
    ASSERT_TRUE(sanitizedRes.isOk());
    EXPECT_EQ(sanitizedRes.value().language, "en");
    EXPECT_EQ(sanitizedRes.value().ui_scale_percent, 125);
    EXPECT_EQ(sanitizedRes.value().card_density, "comfortable");

    // Verify all supported languages, densities, and scale bounds round-trip
    for (const std::string_view lang : SettingsManager::kSupportedLanguages) {
        AppSettings s = defaults;
        s.language = std::string(lang);
        s.card_density = "spacious";
        s.ui_scale_percent = 90;
        auto roundTrip = SettingsManager::deserialize(SettingsManager::serialize(s));
        ASSERT_TRUE(roundTrip.isOk());
        EXPECT_EQ(roundTrip.value().language, lang);
        EXPECT_EQ(roundTrip.value().card_density, "spacious");
        EXPECT_EQ(roundTrip.value().ui_scale_percent, 90);
    }

    // Malformed JSON returns DecodeFailed error
    auto badRes = SettingsManager::deserialize("{ invalid json");
    EXPECT_FALSE(badRes.isOk());
    EXPECT_EQ(badRes.errorCode(), mavira::core::ErrorCode::DecodeFailed);
}

} // namespace
