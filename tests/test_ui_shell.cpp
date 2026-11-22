#include "app/LibraryManager.hpp"
#include "core/cache/CacheManager.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/ExplorerSidebar.hpp"
#include "ui/MainWindow.hpp"
#include "ui/PreferencesWorkspace.hpp"
#include "ui/ToolWorkspaceWidget.hpp"

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {

void writeMinimalCbz(const std::filesystem::path& cbzPath) {
    static constexpr std::uint8_t kTinyPng[] = {
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
    const char name[] = "001.png";
    const std::uint32_t nameLen = 7;
    const std::uint32_t dataSize = sizeof(kTinyPng);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t b : kTinyPng) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1u)));
        }
    }
    crc = ~crc;

    std::vector<std::uint8_t> zip;
    auto push16 = [&](std::uint16_t v) {
        zip.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        zip.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    };
    auto push32 = [&](std::uint32_t v) {
        zip.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        zip.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
        zip.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
        zip.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    };

    push32(0x04034B50u); push16(20); push16(0); push16(0); push16(0); push16(0);
    push32(crc); push32(dataSize); push32(dataSize); push16(nameLen); push16(0);
    zip.insert(zip.end(), name, name + nameLen);
    zip.insert(zip.end(), kTinyPng, kTinyPng + dataSize);

    const std::uint32_t cdOffset = static_cast<std::uint32_t>(zip.size());
    push32(0x02014B50u); push16(20); push16(20); push16(0); push16(0); push16(0); push16(0);
    push32(crc); push32(dataSize); push32(dataSize); push16(nameLen); push16(0);
    push16(0); push16(0); push16(0); push32(0); push32(0);
    zip.insert(zip.end(), name, name + nameLen);

    const std::uint32_t cdSize = static_cast<std::uint32_t>(zip.size()) - cdOffset;
    push32(0x06054B50u); push16(0); push16(0); push16(1); push16(1);
    push32(cdSize); push32(cdOffset); push16(0);

    std::filesystem::create_directories(cbzPath.parent_path());
    std::ofstream ofs(cbzPath, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
}

class UiShellTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempRoot_ = std::filesystem::temp_directory_path() /
                    (L"mavira_ui_shell_test_" + std::to_wstring(stamp));
        std::filesystem::create_directories(tempRoot_);

        paths_ = mavira::data::fs::FileManager::resolvePortablePaths(tempRoot_);
        ASSERT_TRUE(mavira::data::fs::FileManager::ensurePortableDirectories(paths_).isOk());
        ASSERT_TRUE(db_.open(paths_.databasePath).isOk());

        settingsMgr_ = std::make_unique<mavira::data::config::SettingsManager>(paths_.settingsPath);
        cacheMgr_ = std::make_unique<mavira::core::cache::CacheManager>();
        libMgr_ = std::make_unique<mavira::app::LibraryManager>(&db_, cacheMgr_.get(), paths_);
    }

    void TearDown() override {
        if (libMgr_) {
            libMgr_->shutdown();
            libMgr_.reset();
        }
        cacheMgr_.reset();
        settingsMgr_.reset();
        db_.close();
        std::error_code ec;
        std::filesystem::remove_all(tempRoot_, ec);
    }

    std::filesystem::path tempRoot_{};
    mavira::data::fs::PortablePaths paths_{};
    mavira::data::db::DatabaseManager db_{};
    std::unique_ptr<mavira::data::config::SettingsManager> settingsMgr_{};
    std::unique_ptr<mavira::core::cache::CacheManager> cacheMgr_{};
    std::unique_ptr<mavira::app::LibraryManager> libMgr_{};
};

TEST_F(UiShellTest, ColdStartupUnder300MsAndThemeSwitchingPersists) {
    const auto t0 = std::chrono::steady_clock::now();
    mavira::ui::MainWindow win(&db_, cacheMgr_.get(), libMgr_.get(), settingsMgr_.get());
    const auto t1 = std::chrono::steady_clock::now();
    const auto startupMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    win.show();
    QApplication::processEvents();

    EXPECT_LT(startupMs, 300) << "MainWindow cold construction exceeded 300 ms budget";
    EXPECT_EQ(win.currentTheme(), "obsidian_cyber");
    EXPECT_TRUE(win.styleSheet().contains(QStringLiteral("border-radius: 8px;")));
    EXPECT_TRUE(win.styleSheet().contains(QStringLiteral("qlineargradient")));
    EXPECT_TRUE(win.styleSheet().contains(QStringLiteral("#0A0D12")));

    // Top bar has Settings button and no Theme toggle button
    EXPECT_EQ(win.findChild<QPushButton*>(QStringLiteral("BtnThemeToggle")), nullptr);
    auto* btnSettings = win.findChild<QPushButton*>(QStringLiteral("BtnSettings"));
    ASSERT_NE(btnSettings, nullptr);

    // Cycle through all 5 curated gradient themes and verify persistence
    const std::vector<std::pair<std::string, QString>> expectedThemes = {
        {"midnight_velvet", QStringLiteral("#0D0B16")},
        {"warm_parchment", QStringLiteral("#F5EFE4")},
        {"nordic_glacier", QStringLiteral("#E7EFF8")},
        {"sakura_mist", QStringLiteral("#F3EEF5")},
        {"obsidian_cyber", QStringLiteral("#0A0D12")}
    };
    for (const auto& [themeId, expectedHex] : expectedThemes) {
        win.toggleTheme();
        EXPECT_EQ(win.currentTheme(), themeId);
        EXPECT_TRUE(win.styleSheet().contains(expectedHex));
        auto loaded = settingsMgr_->load();
        ASSERT_TRUE(loaded.isOk());
        EXPECT_EQ(loaded.value().theme, themeId);
    }

    // Clicking Settings on top bar opens Preferences workspace
    btnSettings->click();
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Preferences);
}

TEST_F(UiShellTest, SidebarNavigationShelvesSmartviewsSearchContinueReadingAndViewSwitcher) {
    // Seed 3 comics across different series and reading states.
    mavira::data::db::ComicRecord c1;
    c1.filePath = tempRoot_ / L"Library" / L"Akira_Vol1.cbz";
    c1.folderPath = tempRoot_ / L"Library";
    c1.title = "Akira Vol 1";
    c1.series = "Akira";
    c1.author = "Katsuhiro Otomo";
    c1.tags = "Cyberpunk, Sci-Fi";
    c1.format = mavira::core::ComicFormat::CBZ;
    c1.pageCount = 350;
    c1.currentPage = 42; // In progress -> appears in Continue Reading
    c1.lastReadUnix = 1700000100;
    c1.isRead = false;
    auto id1Res = db_.upsertComic(c1);
    ASSERT_TRUE(id1Res.isOk());
    const std::int64_t id1 = id1Res.value();

    mavira::data::db::ComicRecord c2;
    c2.filePath = tempRoot_ / L"Library" / L"Berserk_Vol1.cbz";
    writeMinimalCbz(c2.filePath);
    c2.folderPath = tempRoot_ / L"Library";
    c2.title = "Berserk Vol 1";
    c2.series = "Berserk";
    c2.author = "Kentaro Miura";
    c2.tags = "Dark Fantasy";
    c2.format = mavira::core::ComicFormat::CBZ;
    c2.pageCount = 224;
    c2.currentPage = 0; // Unread -> eligible for Random Unread
    c2.isRead = false;
    auto id2Res = db_.upsertComic(c2);
    ASSERT_TRUE(id2Res.isOk());
    const std::int64_t id2 = id2Res.value();

    mavira::ui::MainWindow win(&db_, cacheMgr_.get(), libMgr_.get(), settingsMgr_.get());
    auto* sidebar = win.sidebar();
    ASSERT_NE(sidebar, nullptr);

    sidebar->refreshNavigation();
    win.refreshLibraryView();

    // 1. All Comics view shows both volumes; Continue Reading Strip shows Akira Vol 1.
    EXPECT_EQ(win.displayedComics().size(), 2u);
    EXPECT_EQ(win.continueReadingList()->count(), 1);

    // 2. Instant FTS5 search filters down to "Otomo".
    win.searchBar()->setText(QStringLiteral("Otomo"));
    EXPECT_EQ(win.displayedComics().size(), 1u);
    EXPECT_EQ(win.displayedComics()[0].id, id1);
    win.searchBar()->clear();
    EXPECT_EQ(win.displayedComics().size(), 2u);

    // 3. Shelves: Create, populate, rename, select, and delete a custom shelf.
    const std::int64_t shelfId = sidebar->createCustomShelfByName(QStringLiteral("Weekend Reads"));
    ASSERT_GT(shelfId, 0);
    ASSERT_TRUE(db_.addComicToShelf(shelfId, id2).isOk());

    EXPECT_TRUE(sidebar->renameCustomShelfById(shelfId, QStringLiteral("Top Fantasy")));

    mavira::ui::SidebarFilterSelection shelfSel;
    shelfSel.kind = mavira::ui::SidebarNodeKind::ShelfItem;
    shelfSel.recordId = shelfId;
    shelfSel.label = QStringLiteral("Top Fantasy");
    emit sidebar->navigationSelected(shelfSel);
    EXPECT_EQ(win.displayedComics().size(), 1u);
    EXPECT_EQ(win.displayedComics()[0].id, id2);

    EXPECT_TRUE(sidebar->deleteCustomShelfById(shelfId));

    // 4. Custom Smartview rule evaluation (Series contains "Akira").
    const QString customRuleJson =
        QStringLiteral(R"({"rules":[{"field":"series","operator":"contains","value":"Akira"}]})");
    const std::int64_t svId =
        sidebar->createCustomSmartView(QStringLiteral("Akira Smartview"), customRuleJson);
    ASSERT_GT(svId, 0);

    mavira::ui::SidebarFilterSelection svSel;
    svSel.kind = mavira::ui::SidebarNodeKind::SmartViewCustom;
    svSel.recordId = svId;
    svSel.label = QStringLiteral("Akira Smartview");
    svSel.payload = customRuleJson;
    emit sidebar->navigationSelected(svSel);
    ASSERT_EQ(win.displayedComics().size(), 1u);
    EXPECT_EQ(win.displayedComics()[0].id, id1);

    EXPECT_TRUE(sidebar->deleteCustomSmartViewById(svId));

    // 5. Random Unread switches to Reader view with the unread volume (Berserk Vol 1).
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Library);
    win.triggerRandomUnread();
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Reader);
    ASSERT_TRUE(win.readerController()->isOpen());

    // Removing the currently open comic from LibraryManager closes it and switches UI back to Library.
    ASSERT_TRUE(libMgr_->removeComicFromLibrary(id2, /*deleteFromDisk=*/false).isOk());
    EXPECT_FALSE(win.readerController()->isOpen());
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Library);

    win.switchToView(mavira::ui::ActiveViewMode::Library);
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Library);
}

TEST_F(UiShellTest, ToolAndPreferencesWorkspacesSwitchAndPersistOptions) {
    mavira::ui::MainWindow win(&db_, cacheMgr_.get(), libMgr_.get(), settingsMgr_.get());
    ASSERT_NE(win.toolWorkspace(), nullptr);
    ASSERT_NE(win.preferencesWorkspace(), nullptr);

    // Open each of the 6 tools and verify header badge & view mode
    const std::vector<std::pair<QString, QString>> expectedTools = {
        {QStringLiteral("convert_comics"), QStringLiteral("CONVERT COMIC BOOKS")},
        {QStringLiteral("convert_images"), QStringLiteral("CONVERT IMAGES")},
        {QStringLiteral("create_comic"), QStringLiteral("CREATE COMIC BOOK")},
        {QStringLiteral("extract_pages"), QStringLiteral("EXTRACT PAGES")},
        {QStringLiteral("extract_ocr"), QStringLiteral("EXTRACT TEXT (NATIVE OCR)")},
        {QStringLiteral("merge_comics"), QStringLiteral("MERGE COMICS")}
    };
    for (const auto& [toolId, badge] : expectedTools) {
        win.openToolWorkspace(toolId);
        EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::ToolWorkspace);
        EXPECT_EQ(win.toolWorkspace()->toolHeaderBadge()->text(), badge);
    }

    // Customize options, close tool, reopen tool, and verify Remember Last Used Options
    win.openToolWorkspace(QStringLiteral("convert_comics"));
    auto opts = win.toolWorkspace()->gatherOptions();
    opts.scaleValue = 75;
    opts.grayscaleManga = true;
    opts.splitIntoFiles = 3;
    opts.rememberLastOptions = true;
    win.toolWorkspace()->applyOptions(opts);
    win.toolWorkspace()->closeTool();
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Library);

    win.openToolWorkspace(QStringLiteral("convert_comics"));
    const auto restored = win.toolWorkspace()->gatherOptions();
    EXPECT_EQ(restored.scaleValue, 75);
    EXPECT_TRUE(restored.grayscaleManga);
    EXPECT_EQ(restored.splitIntoFiles, 3);
    win.toolWorkspace()->closeTool();

    // Clicking a tool in ExplorerSidebar opens ToolWorkspace directly
    emit win.sidebar()->toolRequested(QStringLiteral("extract_pages"));
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::ToolWorkspace);
    EXPECT_EQ(win.toolWorkspace()->toolHeaderBadge()->text(), QStringLiteral("EXTRACT PAGES"));
    win.toolWorkspace()->closeTool();

    // Open Preferences workspace and verify all 8 interactive pages & back button
    win.openPreferencesWorkspace();
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Preferences);
    EXPECT_EQ(win.preferencesWorkspace()->categoryStack()->count(), 8);
    win.preferencesWorkspace()->backButton()->click();
    EXPECT_EQ(win.activeViewMode(), mavira::ui::ActiveViewMode::Library);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
