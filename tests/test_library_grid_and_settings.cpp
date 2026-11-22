#include "app/LibraryManager.hpp"
#include "core/archive/ArchiveEngine.hpp"
#include "core/cache/CacheManager.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/ComicItemDelegate.hpp"
#include "ui/LibraryView.hpp"
#include "ui/LocalizationManager.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SettingsDialog.hpp"

#include <gtest/gtest.h>
#include <zip.h>

#include <QApplication>
#include <QCheckBox>
#include <QEventLoop>
#include <QImage>
#include <QKeySequenceEdit>
#include <QPainter>
#include <QPixmap>
#include <QRadioButton>
#include <QScrollBar>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

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

void createTestCbz(const std::filesystem::path& cbzPath, int pageCount) {
    std::filesystem::create_directories(cbzPath.parent_path());
    int errCode = 0;
    const std::string utf8Path = mavira::data::fs::FileManager::wideToUtf8(cbzPath.native());
    zip_t* archive = zip_open(utf8Path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errCode);
    ASSERT_NE(archive, nullptr);
    for (int i = 1; i <= pageCount; ++i) {
        const std::string name = "page_" + std::to_string(i) + ".png";
        zip_source_t* src = zip_source_buffer(archive, kTinyPngBytes, sizeof(kTinyPngBytes), 0);
        ASSERT_NE(src, nullptr);
        ASSERT_GE(zip_file_add(archive, name.c_str(), src, ZIP_FL_OVERWRITE), 0);
    }
    ASSERT_EQ(zip_close(archive), 0);
}

class LibraryGridAndSettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempRoot_ = std::filesystem::temp_directory_path() /
                    (L"mavira_grid_settings_test_" + std::to_wstring(stamp));
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

TEST_F(LibraryGridAndSettingsTest, Virtualized30000ItemGridScrollsAt60FpsAndPaintsBadges) {
    mavira::ui::LibraryView view(&db_, cacheMgr_.get(), libMgr_.get());
    view.resize(1024, 720);
    view.show();
    view.setDisplayOptions(/*showCircularProgress=*/true, /*smartRename=*/true);

    ASSERT_EQ(view.listView()->viewMode(), QListView::IconMode);
    ASSERT_TRUE(view.listView()->uniformItemSizes());
    ASSERT_EQ(view.listView()->layoutMode(), QListView::Batched);

    std::vector<mavira::data::db::ComicRecord> records;
    records.reserve(30500);
    for (int i = 0; i < 30500; ++i) {
        mavira::data::db::ComicRecord r;
        r.id = i + 1;
        r.filePath = tempRoot_ / (L"Vol_" + std::to_wstring(i) + L".cbz");
        r.folderPath = tempRoot_;
        r.title = "[Group] Series Title Vol " + std::to_string(i + 1) + " (Digital)";
        r.format = mavira::core::ComicFormat::CBZ;
        r.pageCount = 100;
        r.currentPage = (i % 3 == 0) ? 45 : 0;
        r.isRead = (i % 5 == 0);
        r.isOfflinePinned = (i % 7 == 0);
        records.push_back(std::move(r));
    }

    const auto loadT0 = std::chrono::steady_clock::now();
    view.setComics(std::move(records));
    QApplication::processEvents();
    const auto loadT1 = std::chrono::steady_clock::now();
    const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(loadT1 - loadT0).count();
    EXPECT_EQ(view.model()->rowCount(), 30500);
    EXPECT_LT(loadMs, 500);

    // Verify Smart Rename cleaned title
    const QString firstTitle = view.model()->data(view.model()->index(0), Qt::DisplayRole).toString();
    EXPECT_EQ(firstTitle, QStringLiteral("Series Title Vol 1"));

    // Verify 60fps scrolling (< 16 ms per scroll+paint frame) across 20 scroll steps
    auto* vbar = view.listView()->verticalScrollBar();
    const int maxScroll = std::max(1, vbar->maximum());
    const auto scrollT0 = std::chrono::steady_clock::now();
    for (int step = 1; step <= 20; ++step) {
        vbar->setValue((maxScroll * step) / 20);
        view.listView()->viewport()->repaint();
    }
    const auto scrollT1 = std::chrono::steady_clock::now();
    const double avgFrameMs =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(scrollT1 - scrollT0).count()) /
        20000.0;
    EXPECT_LT(avgFrameMs, 16.0) << "Average scroll+paint frame exceeded 16 ms (60fps) budget";

    // Explicitly exercise ComicItemDelegate::paint for circular progress + badges
    QImage canvas(
        mavira::ui::ComicItemDelegate::kCardWidth,
        mavira::ui::ComicItemDelegate::kCardHeight,
        QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    QStyleOptionViewItem opt;
    opt.rect = canvas.rect();
    view.delegate()->paint(&painter, opt, view.model()->index(0));
    painter.end();
}

TEST_F(LibraryGridAndSettingsTest, NestedImportMode1AndMode2CoverSwitchingAndPackPurge) {
    const std::filesystem::path nestedDir = tempRoot_ / L"NestedComics";
    createTestCbz(nestedDir / L"Manga" / L"SeriesA" / L"Vol01.cbz", 3);
    createTestCbz(nestedDir / L"Manga" / L"SeriesB" / L"Vol02.cbz", 2);

    mavira::ui::MainWindow win(&db_, cacheMgr_.get(), libMgr_.get(), settingsMgr_.get());
    win.show();

    QEventLoop loop;
    bool scanDone = false;
    QObject::connect(
        libMgr_.get(),
        &mavira::app::LibraryManager::scanFinished,
        &loop,
        [&](int, int, bool) {
            scanDone = true;
            loop.quit();
        });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);

    win.addFolderPathAsync(nestedDir);
    loop.exec();
    ASSERT_TRUE(scanDone);
    ASSERT_EQ(win.displayedComics().size(), 2u);

    // Wait > 75 ms debounce so Mode 1 visible covers load asynchronously
    QEventLoop coverWait;
    QTimer::singleShot(250, &coverWait, &QEventLoop::quit);
    coverWait.exec();

    // Switch to Mode 2 (covers.pack) via SettingsDialog + applySettings
    auto s = win.currentSettings();
    s.cover_cache_mode = 2;
    s.circular_progress_bar = true;
    s.smart_rename = true;
    win.applySettings(s, /*persist=*/true);

    EXPECT_NE(
        dynamic_cast<mavira::core::cache::PackFileCoverProvider*>(cacheMgr_->coverStorage()),
        nullptr);

    // Build covers.pack via SettingsDialog and then purge it
    mavira::ui::SettingsDialog dlg(s, &db_, cacheMgr_.get(), paths_);
    QEventLoop packLoop;
    bool built = false;
    QObject::connect(&dlg, &mavira::ui::SettingsDialog::coversPackBuilt, &packLoop, [&]() {
        built = true;
        packLoop.quit();
    });
    QTimer::singleShot(5000, &packLoop, &QEventLoop::quit);
    dlg.buildCoversPack();
    packLoop.exec();

    ASSERT_TRUE(built);
    EXPECT_TRUE(std::filesystem::exists(paths_.coversPackPath));

    dlg.purgeCoverCache();
    EXPECT_FALSE(std::filesystem::exists(paths_.coversPackPath));
}

TEST_F(LibraryGridAndSettingsTest, SettingsDialogTogglesAndShortcutsRoundTrip) {
    mavira::data::config::AppSettings initial;
    initial.default_manga_mode = false;
    initial.smart_rename = true;
    initial.cover_cache_mode = 2;

    mavira::ui::SettingsDialog dlg(initial, &db_, cacheMgr_.get(), paths_);
    EXPECT_EQ(dlg.tabWidget()->count(), 3);
    EXPECT_TRUE(dlg.mode2Radio()->isChecked());

    const auto checkboxes = dlg.findChildren<QCheckBox*>();
    EXPECT_EQ(checkboxes.size(), 9);

    // Flip all 9 toggle switches
    for (auto* cb : checkboxes) {
        cb->setChecked(!cb->isChecked());
    }
    dlg.mode1Radio()->setChecked(true);

    const auto updated = dlg.settings();
    EXPECT_EQ(updated.default_manga_mode, !initial.default_manga_mode);
    EXPECT_EQ(updated.smart_rename, !initial.smart_rename);
    EXPECT_EQ(updated.cover_cache_mode, 1);

    // Modify a shortcut and then reset to defaults
    const auto keyEdits = dlg.findChildren<QKeySequenceEdit*>();
    ASSERT_FALSE(keyEdits.isEmpty());
    keyEdits.front()->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Shift+X")));
    dlg.resetShortcutsToDefaults();
    const auto resetSettings = dlg.settings();
    EXPECT_EQ(resetSettings.shortcuts, mavira::data::config::AppSettings{}.shortcuts);
}

TEST_F(LibraryGridAndSettingsTest, PerToolPresetsAndRememberLastUsedOptions) {
    for (const auto toolId : mavira::data::config::SettingsManager::kToolIds) {
        EXPECT_TRUE(mavira::data::config::SettingsManager::isValidToolId(toolId));
    }
    EXPECT_FALSE(mavira::data::config::SettingsManager::isValidToolId("unknown_tool"));

    // Save custom options for convert_comics with rememberLastOptions = true
    mavira::core::ToolExecutionOptions comicOpts;
    comicOpts.inputPaths = {L"C:\\Comics\\Vol1.cbz", L"C:\\Comics\\Vol2.cbr"};
    comicOpts.recursiveFolders = false;
    comicOpts.outputFormat = "pdf";
    comicOpts.outputFolderMode = "custom";
    comicOpts.customOutputDir = L"D:\\Output";
    comicOpts.ifExistsPolicy = "overwrite";
    comicOpts.splitIntoFiles = 3;
    comicOpts.compressionMode = "max";
    comicOpts.pdfCreationDpi = 300;
    comicOpts.scaleMode = "max_width";
    comicOpts.scaleValue = 1600;
    comicOpts.targetImageFormat = "webp";
    comicOpts.enableLevels = true;
    comicOpts.blackLevel = 12;
    comicOpts.whiteLevel = 240;
    comicOpts.brightness = 8;
    comicOpts.sharpness = 30;
    comicOpts.grayscaleManga = true;
    comicOpts.cropBorderPx = 5;
    comicOpts.autoCropUniform = true;
    comicOpts.addBorderPx = 2;
    comicOpts.borderColorHex = "#FFFFFF";
    comicOpts.workerThreads = 6;
    comicOpts.webpLossless = true;
    comicOpts.pageRange = "1-10, 15";
    comicOpts.pageNumberDigits = 4;
    comicOpts.createFolderMode = "single_combined";
    comicOpts.stripImageMetadata = true;
    comicOpts.ocrLanguage = "ja";
    comicOpts.ocrUpscaleFactor = 1.5;
    comicOpts.ocrOutputFormat = "md";
    comicOpts.ocrIncludePageHeaders = false;
    comicOpts.ocrFilterNoiseSymbols = false;
    comicOpts.ocrNormalizeAllCaps = false;
    comicOpts.mergeNormalizeWidth = true;
    comicOpts.rememberLastOptions = true;

    ASSERT_TRUE(settingsMgr_->saveToolOptions("convert_comics", comicOpts).isOk());

    auto loadedRes = settingsMgr_->loadToolOptions("convert_comics");
    ASSERT_TRUE(loadedRes.isOk()) << loadedRes.error().message;
    EXPECT_EQ(loadedRes.value(), comicOpts);

    // Other tools remain independent defaults
    auto ocrRes = settingsMgr_->loadToolOptions("extract_ocr");
    ASSERT_TRUE(ocrRes.isOk());
    EXPECT_EQ(ocrRes.value(), mavira::core::ToolExecutionOptions{});

    // When rememberLastOptions == false, next load returns default options (with rememberLastOptions = false)
    mavira::core::ToolExecutionOptions imgOpts;
    imgOpts.outputFormat = "png";
    imgOpts.scaleValue = 50;
    imgOpts.rememberLastOptions = false;
    ASSERT_TRUE(settingsMgr_->saveToolOptions("convert_images", imgOpts).isOk());

    auto loadedImg = settingsMgr_->loadToolOptions("convert_images");
    ASSERT_TRUE(loadedImg.isOk());
    EXPECT_FALSE(loadedImg.value().rememberLastOptions);
    EXPECT_EQ(loadedImg.value().scaleValue, 100);
    EXPECT_EQ(loadedImg.value().outputFormat, "cbz");

    // Resetting tool options restores full defaults including rememberLastOptions = true
    ASSERT_TRUE(settingsMgr_->resetToolOptions("convert_comics").isOk());
    auto resetRes = settingsMgr_->loadToolOptions("convert_comics");
    ASSERT_TRUE(resetRes.isOk());
    EXPECT_EQ(resetRes.value(), mavira::core::ToolExecutionOptions{});
}

TEST_F(LibraryGridAndSettingsTest, AppearanceSettingsWireLocalizationScaleAndDensity) {
    using mavira::data::config::AppSettings;
    using mavira::ui::LocalizationManager;
    using mavira::ui::MainWindow;

    MainWindow win(&db_, cacheMgr_.get(), libMgr_.get(), settingsMgr_.get());

    // 1. Localization across all 5 languages
    AppSettings s = win.currentSettings();
    s.language = "id";
    s.ui_scale_percent = 125;
    s.card_density = "compact";
    win.applySettings(s, /*persist=*/false);
    EXPECT_EQ(LocalizationManager::currentLanguage(), "id");
    EXPECT_EQ(LocalizationManager::tr("btn.library"), QStringLiteral("Pustaka"));
    EXPECT_EQ(win.libraryView()->listView()->gridSize(), QSize(150, 225));

    s.language = "ja";
    s.card_density = "spacious";
    win.applySettings(s, /*persist=*/false);
    EXPECT_EQ(LocalizationManager::currentLanguage(), "ja");
    EXPECT_EQ(LocalizationManager::tr("btn.library"), QString::fromUtf8("\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe3\x83\xa9\xe3\x83\xaa"));
    EXPECT_EQ(win.libraryView()->listView()->gridSize(), QSize(225, 330));

    s.language = "ko";
    s.card_density = "comfortable";
    win.applySettings(s, /*persist=*/false);
    EXPECT_EQ(LocalizationManager::currentLanguage(), "ko");
    EXPECT_EQ(LocalizationManager::tr("btn.library"), QString::fromUtf8("\xeb\x9d\xbc\xec\x9d\xb4\xeb\xb8\x8c\xeb\x9f\xac\xeb\xa6\xac"));
    EXPECT_EQ(win.libraryView()->listView()->gridSize(), QSize(185, 275));

    s.language = "zh";
    win.applySettings(s, /*persist=*/false);
    EXPECT_EQ(LocalizationManager::currentLanguage(), "zh");
    EXPECT_EQ(LocalizationManager::tr("btn.library"), QString::fromUtf8("\xe4\xb9\xa6\xe5\xba\x93"));

    s.language = "en";
    s.ui_scale_percent = 100;
    win.applySettings(s, /*persist=*/false);
    EXPECT_EQ(LocalizationManager::currentLanguage(), "en");
    EXPECT_EQ(LocalizationManager::tr("btn.library"), QStringLiteral("Library"));

    // 2. UI Scale QSS includes scaled font sizes
    const QString qss125 = MainWindow::buildStylesheet("obsidian_cyber", 125);
    EXPECT_TRUE(qss125.contains(QStringLiteral("font-size: 11.9pt;")));
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
