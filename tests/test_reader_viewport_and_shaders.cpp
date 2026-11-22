#include "app/LibraryManager.hpp"
#include "app/ReaderController.hpp"
#include "core/cache/CacheManager.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "core/rendering/RenderingEngine.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/MainWindow.hpp"
#include "ui/ReaderViewport.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <gtest/gtest.h>
#include <zip.h>

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using mavira::app::LibraryManager;
using mavira::app::ReaderController;
using mavira::core::ImageFrame;
using mavira::core::cache::CacheManager;
using mavira::core::imaging::ImageDecoder;
using mavira::core::rendering::ReadingDirection;
using mavira::core::rendering::ReadingMode;
using mavira::data::config::SettingsManager;
using mavira::data::db::ComicRecord;
using mavira::data::db::DatabaseManager;
using mavira::ui::ActiveViewMode;
using mavira::ui::MainWindow;
using mavira::ui::ReaderViewport;

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

// Minimal valid 1-page PDF document.
constexpr char kMinimalPdf[] =
    "%PDF-1.4\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >> endobj\n"
    "xref\n"
    "0 4\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "trailer << /Size 4 /Root 1 0 R >>\n"
    "startxref\n"
    "190\n"
    "%%EOF\n";

template <typename Pred>
bool waitUntil(Pred&& pred, int timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents(QEventLoop::AllEvents, 15);
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QApplication::processEvents(QEventLoop::AllEvents, 15);
    return pred();
}

std::size_t currentProcessWorkingSetMb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                               sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024ULL * 1024ULL);
    }
    return 0;
}

ImageFrame makeTestImage(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    ImageFrame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.pixels.resize(static_cast<std::size_t>(f.stride) * h);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4) {
        f.pixels[i + 0] = r;
        f.pixels[i + 1] = g;
        f.pixels[i + 2] = b;
        f.pixels[i + 3] = 255;
    }
    return f;
}

class ReaderViewportTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempRoot_ = std::filesystem::temp_directory_path() /
                    (L"mavira_viewport_test_" + std::to_wstring(stamp));
        std::filesystem::create_directories(tempRoot_);

        paths_ = mavira::data::fs::FileManager::resolvePortablePaths(tempRoot_);
        ASSERT_TRUE(mavira::data::fs::FileManager::ensurePortableDirectories(paths_).isOk());
        ASSERT_TRUE(db_.open(paths_.databasePath).isOk());

        settingsMgr_ = std::make_unique<SettingsManager>(paths_.settingsPath);
        cacheMgr_ = std::make_unique<CacheManager>();
        libMgr_ = std::make_unique<LibraryManager>(&db_, cacheMgr_.get(), paths_);
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
    DatabaseManager db_{};
    std::unique_ptr<SettingsManager> settingsMgr_{};
    std::unique_ptr<CacheManager> cacheMgr_{};
    std::unique_ptr<LibraryManager> libMgr_{};
};

TEST_F(ReaderViewportTest, ShadersCompileAndViewportSupportsSpreadWebtoonLoupeAndClearPage) {
    // Build a CBZ containing normal portrait pages and one >4096px Webtoon strip.
    const std::filesystem::path cbzPath = tempRoot_ / L"shader_test.cbz";
    int errCode = 0;
    const std::string utf8Path = mavira::data::fs::FileManager::wideToUtf8(cbzPath.native());
    zip_t* archive = zip_open(utf8Path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errCode);
    ASSERT_NE(archive, nullptr);

    auto addPngPage = [&](const std::string& name, const std::vector<std::uint8_t>& bytes) {
        zip_source_t* src = zip_source_buffer(archive, bytes.data(), bytes.size(), 0);
        ASSERT_NE(src, nullptr);
        ASSERT_GE(zip_file_add(archive, name.c_str(), src, ZIP_FL_OVERWRITE), 0);
    };

    const auto p0 = ImageDecoder::encodeJpeg(makeTestImage(200, 300, 220, 220, 220), 90);
    const auto p1 = ImageDecoder::encodeJpeg(makeTestImage(200, 300, 40, 40, 40), 90);
    const auto webtoonStrip = ImageDecoder::encodeJpeg(makeTestImage(120, 4500, 120, 180, 240), 90);
    ASSERT_TRUE(p0.isOk() && p1.isOk() && webtoonStrip.isOk());

    addPngPage("001.jpg", p0.value());
    addPngPage("002.jpg", p1.value());
    addPngPage("003.jpg", webtoonStrip.value());
    ASSERT_EQ(zip_close(archive), 0);

    MainWindow win(&db_, cacheMgr_.get(), libMgr_.get(), settingsMgr_.get());
    win.resize(960, 640);
    win.show();
    QApplication::processEvents();

    ComicRecord rec;
    rec.filePath = cbzPath;
    rec.title = "Shader & Viewport Test";
    rec.pageCount = 3;
    win.openComicInReader(rec);

    ASSERT_EQ(win.activeViewMode(), ActiveViewMode::Reader);
    auto* controller = win.readerController();
    auto* viewport = win.readerViewport();
    ASSERT_NE(controller, nullptr);
    ASSERT_NE(viewport, nullptr);

    // Wait for page 0 to decode and layout.
    ASSERT_TRUE(waitUntil([&]() {
        return controller->currentLayout().has_value() && controller->isPageCached(0);
    }));

    // Trigger OpenGL initialization & frame render.
    const QImage frame0 = viewport->grabFramebuffer();
    EXPECT_FALSE(frame0.isNull());
    EXPECT_TRUE(viewport->shadersCompiled());
    EXPECT_EQ(viewport->uploadedTileCount(), 1u);

    // 1. RTL & LTR Dual-Page Spread modes.
    controller->setReadingMode(ReadingMode::DualPage);
    controller->setReadingDirection(ReadingDirection::RTL);
    ASSERT_TRUE(waitUntil([&]() {
        return controller->currentLayout().has_value() && controller->currentLayout()->isDualSpread;
    }));
    const QImage rtlFrame = viewport->grabFramebuffer();
    EXPECT_FALSE(rtlFrame.isNull());

    controller->setReadingDirection(ReadingDirection::LTR);
    const QImage ltrFrame = viewport->grabFramebuffer();
    EXPECT_FALSE(ltrFrame.isNull());

    // 2. Hold-to-Zoom Loupe Magnifier (right-click press/move/release, 2.5x-3.5x).
    QMouseEvent rPress(
        QEvent::MouseButtonPress, QPointF(400.0, 300.0), QPointF(400.0, 300.0),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &rPress);
    EXPECT_TRUE(viewport->isLoupeActive());
    viewport->setLoupeZoom(3.2f);
    EXPECT_FLOAT_EQ(viewport->loupeZoom(), 3.2f);
    const QImage loupeFrame = viewport->grabFramebuffer();
    EXPECT_FALSE(loupeFrame.isNull());

    QMouseEvent rRelease(
        QEvent::MouseButtonRelease, QPointF(400.0, 300.0), QPointF(400.0, 300.0),
        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &rRelease);
    EXPECT_FALSE(viewport->isLoupeActive());

    // 3. Vertical Webtoon mode (> 4096 px image -> 3 vertical GPU tiles).
    controller->setReadingMode(ReadingMode::VerticalWebtoon);
    ASSERT_TRUE(controller->goToPage(2));
    ASSERT_TRUE(waitUntil([&]() {
        return controller->currentLayout().has_value() &&
               controller->currentLayout()->tiles.size() == 3u;
    }));
    const QImage webtoonFrame = viewport->grabFramebuffer();
    EXPECT_FALSE(webtoonFrame.isNull());
    EXPECT_EQ(viewport->uploadedTileCount(), 3u);

    // 4. HUD Overlay toggle & clearPage().
    viewport->showHud();
    EXPECT_TRUE(viewport->isHudVisible());
    viewport->hideHud();
    EXPECT_FALSE(viewport->isHudVisible());

    viewport->clearPage();
    EXPECT_EQ(viewport->uploadedTileCount(), 0u);
}

TEST_F(ReaderViewportTest, OpensCbzAndPdfWithLowRamFootprintUnder185Mb) {
    // Create a valid CBZ and a valid PDF and verify ReaderController + ReaderViewport
    // render both while keeping process working-set RAM well under 185 MB.
    const std::filesystem::path cbzPath = tempRoot_ / L"sample_volume.cbz";
    {
        int errCode = 0;
        const std::string utf8Path = mavira::data::fs::FileManager::wideToUtf8(cbzPath.native());
        zip_t* archive = zip_open(utf8Path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errCode);
        ASSERT_NE(archive, nullptr);
        for (int i = 1; i <= 5; ++i) {
            const std::string name = "p" + std::to_string(i) + ".png";
            zip_source_t* src = zip_source_buffer(archive, kTinyPngBytes, sizeof(kTinyPngBytes), 0);
            ASSERT_NE(src, nullptr);
            ASSERT_GE(zip_file_add(archive, name.c_str(), src, ZIP_FL_OVERWRITE), 0);
        }
        ASSERT_EQ(zip_close(archive), 0);
    }

    const std::filesystem::path pdfPath = tempRoot_ / L"sample_document.pdf";
    {
        std::ofstream pdfOut(pdfPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(pdfOut.is_open());
        pdfOut.write(kMinimalPdf, sizeof(kMinimalPdf) - 1);
        ASSERT_TRUE(pdfOut.good());
    }

    MainWindow win(&db_, cacheMgr_.get(), libMgr_.get(), settingsMgr_.get());
    win.resize(800, 600);
    win.show();
    QApplication::processEvents();

    ComicRecord cbzRec;
    cbzRec.filePath = cbzPath;
    cbzRec.title = "Sample CBZ";
    cbzRec.pageCount = 5;
    win.openComicInReader(cbzRec);

    ASSERT_TRUE(waitUntil([&]() {
        return win.readerController()->isPageCached(0);
    }));
    EXPECT_FALSE(win.readerViewport()->grabFramebuffer().isNull());

    ComicRecord pdfRec;
    pdfRec.filePath = pdfPath;
    pdfRec.title = "Sample PDF";
    pdfRec.pageCount = 1;
    win.openComicInReader(pdfRec);

    ASSERT_TRUE(waitUntil([&]() {
        return win.readerController()->isPageCached(0) &&
               win.readerController()->currentLayout().has_value();
    }));
    EXPECT_FALSE(win.readerViewport()->grabFramebuffer().isNull());

    const std::size_t ramMb = currentProcessWorkingSetMb();
    EXPECT_GT(ramMb, 0u);
    EXPECT_LE(ramMb, 185u) << "Working set RAM exceeded 185 MB budget: " << ramMb << " MB";
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
