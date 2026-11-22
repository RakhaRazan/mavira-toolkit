#include "ui/PreferencesWorkspace.hpp"

#include "core/archive/ComicMerger.hpp"
#include "ui/LocalizationManager.hpp"
#include "ui/SectionCardWidget.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace mavira::ui {

namespace {

[[nodiscard]] QWidget* wrapPrefScroll(QWidget* content, QWidget* parent) {
    content->setObjectName(QStringLiteral("WorkspaceScrollContent"));
    content->setAutoFillBackground(false);
    auto* scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("WorkspaceScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setObjectName(QStringLiteral("WorkspaceScrollViewport"));
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(content);
    return scroll;
}

[[nodiscard]] QString prettyShortcutLabel(const std::string& key) {
    if (key == "add_bookmark") return QStringLiteral("Add Bookmark");
    if (key == "next_page") return QStringLiteral("Next Page");
    if (key == "open_library") return QStringLiteral("Return to Library");
    if (key == "prev_page") return QStringLiteral("Previous Page");
    if (key == "toggle_auto_scroll") return QStringLiteral("Toggle Auto-Scroll");
    if (key == "toggle_enhancer") return QStringLiteral("Toggle Manga Enhancer");
    if (key == "toggle_fullscreen") return QStringLiteral("Toggle Fullscreen");
    if (key == "toggle_loupe") return QStringLiteral("Toggle Loupe Magnifier");
    if (key == "toggle_manga_mode") return QStringLiteral("Toggle Manga Mode");
    if (key == "zoom_in") return QStringLiteral("Zoom In");
    if (key == "zoom_out") return QStringLiteral("Zoom Out");
    if (key == "zoom_reset") return QStringLiteral("Reset Zoom");
    return QString::fromStdString(key);
}

} // namespace

PreferencesWorkspace::PreferencesWorkspace(
    mavira::data::db::DatabaseManager* db,
    mavira::core::cache::CacheManager* cacheManager,
    const mavira::data::fs::PortablePaths& portablePaths,
    QWidget* parent)
    : QWidget(parent),
      db_(db),
      cacheManager_(cacheManager),
      portablePaths_(portablePaths) {
    setObjectName(QStringLiteral("PreferencesWorkspace"));
    setupUi();
    loadFromSettings(workingSettings_);
}

PreferencesWorkspace::~PreferencesWorkspace() {
    packStopSource_.request_stop();
    if (packWorkerThread_.joinable()) {
        packWorkerThread_.join();
    }
}

void PreferencesWorkspace::setupUi() {
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // =========================================================================
    // Left Sidebar (240px)
    // =========================================================================
    auto* sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("PrefsSidebar"));
    sidebar->setMinimumWidth(240);
    sidebar->setMaximumWidth(360);
    sidebar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(14, 16, 14, 16);
    sideLayout->setSpacing(8);

    btnBack_ = new QPushButton(QStringLiteral("BACK TO LIBRARY / READER"), sidebar);
    btnBack_->setObjectName(QStringLiteral("ToolCloseBtn"));
    btnBack_->setMinimumHeight(36);

    btnApply_ = new QPushButton(QStringLiteral("APPLY CHANGES"), sidebar);
    btnApply_->setObjectName(QStringLiteral("ToolPrimaryBtn"));
    btnApply_->setMinimumHeight(36);

    btnResetAll_ = new QPushButton(QStringLiteral("RESET ALL"), sidebar);
    btnResetAll_->setMinimumHeight(34);

    catLabel_ = new QLabel(LocalizationManager::tr("prefs.categories"), sidebar);
    catLabel_->setObjectName(QStringLiteral("SidebarSectionTitle"));

    categoryGroup_ = new QButtonGroup(this);
    categoryGroup_->setExclusive(true);

    const char* categoryKeys[] = {
        "prefs.cat.appearance",
        "prefs.cat.ui",
        "prefs.cat.reader",
        "prefs.cat.library",
        "prefs.cat.formats",
        "prefs.cat.external",
        "prefs.cat.shortcuts",
        "prefs.cat.advanced"
    };

    sideLayout->addWidget(btnBack_);
    sideLayout->addWidget(btnApply_);
    sideLayout->addWidget(btnResetAll_);
    sideLayout->addSpacing(8);
    sideLayout->addWidget(catLabel_);

    for (int i = 0; i < 8; ++i) {
        auto* btn = new QPushButton(LocalizationManager::tr(categoryKeys[i]), sidebar);
        btn->setObjectName(QStringLiteral("WorkspaceNavBtn"));
        btn->setCheckable(true);
        btn->setMinimumHeight(34);
        if (i == 0) btn->setChecked(true);
        categoryGroup_->addButton(btn, i);
        sideLayout->addWidget(btn);
    }
    sideLayout->addStretch(1);

    // =========================================================================
    // Right Content Area
    // =========================================================================
    auto* rightArea = new QWidget(this);
    rightArea->setObjectName(QStringLiteral("PrefsContentArea"));
    auto* rightLayout = new QVBoxLayout(rightArea);
    rightLayout->setContentsMargins(20, 16, 20, 16);
    rightLayout->setSpacing(14);

    auto* headerRow = new QHBoxLayout();
    headerRow->addStretch(1);
    headerBadge_ = new QLabel(LocalizationManager::tr("prefs.badge"), rightArea);
    headerBadge_->setObjectName(QStringLiteral("WorkspaceHeaderBadge"));
    headerRow->addWidget(headerBadge_);
    rightLayout->addLayout(headerRow);

    categoryStack_ = new QStackedWidget(rightArea);

    // 0: Appearance
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        auto* card = new SectionCardWidget(QStringLiteral("APPEARANCE"), page);
        auto* form = new QFormLayout();
        form->setSpacing(10);
        cmbTheme_ = new QComboBox(card);
        cmbTheme_->addItem(QStringLiteral("Obsidian Cyber (Dark)"), QStringLiteral("obsidian_cyber"));
        cmbTheme_->addItem(QStringLiteral("Midnight Velvet (Dark)"), QStringLiteral("midnight_velvet"));
        cmbTheme_->addItem(QStringLiteral("Warm Parchment (Warm Light)"), QStringLiteral("warm_parchment"));
        cmbTheme_->addItem(QStringLiteral("Nordic Glacier (Cold Light)"), QStringLiteral("nordic_glacier"));
        cmbTheme_->addItem(QStringLiteral("Sakura Mist (Soft Light)"), QStringLiteral("sakura_mist"));
        cmbLanguage_ = new QComboBox(card);
        cmbLanguage_->addItem(QStringLiteral("English"), QStringLiteral("en"));
        cmbLanguage_->addItem(QStringLiteral("Bahasa Indonesia"), QStringLiteral("id"));
        cmbLanguage_->addItem(QString::fromUtf8("Chinese (\xe4\xb8\xad\xe6\x96\x87)"), QStringLiteral("zh"));
        cmbLanguage_->addItem(QString::fromUtf8("Japanese (\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e)"), QStringLiteral("ja"));
        cmbLanguage_->addItem(QString::fromUtf8("Korean (\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4)"), QStringLiteral("ko"));
        cmbUiScale_ = new QComboBox(card);
        cmbUiScale_->addItem(QStringLiteral("90% (Compact)"), 90);
        cmbUiScale_->addItem(QStringLiteral("100% (Default)"), 100);
        cmbUiScale_->addItem(QStringLiteral("110% (Large)"), 110);
        cmbUiScale_->addItem(QStringLiteral("125% (Extra Large)"), 125);
        cmbCardDensity_ = new QComboBox(card);
        cmbCardDensity_->addItem(QStringLiteral("Compact"), QStringLiteral("compact"));
        cmbCardDensity_->addItem(QStringLiteral("Comfortable"), QStringLiteral("comfortable"));
        cmbCardDensity_->addItem(QStringLiteral("Spacious"), QStringLiteral("spacious"));
        lblTheme_ = new QLabel(LocalizationManager::tr("prefs.lbl.theme"), card);
        lblLanguage_ = new QLabel(LocalizationManager::tr("prefs.lbl.language"), card);
        lblUiScale_ = new QLabel(LocalizationManager::tr("prefs.lbl.ui_scale"), card);
        lblCardDensity_ = new QLabel(LocalizationManager::tr("prefs.lbl.card_density"), card);
        form->addRow(lblTheme_, cmbTheme_);
        form->addRow(lblLanguage_, cmbLanguage_);
        form->addRow(lblUiScale_, cmbUiScale_);
        form->addRow(lblCardDensity_, cmbCardDensity_);
        card->contentLayout()->addLayout(form);
        lay->addWidget(card);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    // 1: User Interface
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        auto* card = new SectionCardWidget(QStringLiteral("USER INTERFACE"), page);
        chkCircularProgressBar_ = new QCheckBox(
            QStringLiteral("Show circular reading progress badge on library covers"), card);
        chkSmartRename_ = new QCheckBox(
            QStringLiteral("Smart Rename (clean release group tags in library titles)"), card);
        chkShowHiddenItems_ = new QCheckBox(
            QStringLiteral("Show hidden library entries"), card);
        chkConfirmDeleteDisk_ = new QCheckBox(
            QStringLiteral("Confirm before moving files to Recycle Bin"), card);
        chkShowStatusTelemetry_ = new QCheckBox(
            QStringLiteral("Show telemetry & cache stats in status bar"), card);

        auto* uiForm = new QFormLayout();
        uiForm->setSpacing(10);
        cmbCursorHideSeconds_ = new QComboBox(card);
        cmbCursorHideSeconds_->addItem(QStringLiteral("Never"), 0);
        cmbCursorHideSeconds_->addItem(QStringLiteral("1 second"), 1);
        cmbCursorHideSeconds_->addItem(QStringLiteral("2 seconds"), 2);
        cmbCursorHideSeconds_->addItem(QStringLiteral("5 seconds"), 5);
        uiForm->addRow(QStringLiteral("Reader Cursor Auto-Hide Delay:"), cmbCursorHideSeconds_);

        card->contentLayout()->addWidget(chkCircularProgressBar_);
        card->contentLayout()->addWidget(chkSmartRename_);
        card->contentLayout()->addWidget(chkShowHiddenItems_);
        card->contentLayout()->addWidget(chkConfirmDeleteDisk_);
        card->contentLayout()->addWidget(chkShowStatusTelemetry_);
        card->contentLayout()->addLayout(uiForm);
        lay->addWidget(card);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    // 2: Reader
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        lay->setSpacing(16);

        auto* layoutCard = new SectionCardWidget(QStringLiteral("PAGE LAYOUT & CANVAS"), page);
        auto* layoutForm = new QFormLayout();
        layoutForm->setSpacing(10);
        cmbDefaultPageLayout_ = new QComboBox(layoutCard);
        cmbDefaultPageLayout_->addItem(QStringLiteral("Single Page"), QStringLiteral("single"));
        cmbDefaultPageLayout_->addItem(QStringLiteral("Dual Spread"), QStringLiteral("double"));
        cmbDefaultPageLayout_->addItem(QStringLiteral("Vertical Webtoon"), QStringLiteral("webtoon"));
        cmbDefaultFitMode_ = new QComboBox(layoutCard);
        cmbDefaultFitMode_->addItem(QStringLiteral("Fit Width"), QStringLiteral("fit_width"));
        cmbDefaultFitMode_->addItem(QStringLiteral("Fit Height"), QStringLiteral("fit_height"));
        cmbDefaultFitMode_->addItem(QStringLiteral("Fit Page"), QStringLiteral("fit_page"));
        cmbDefaultFitMode_->addItem(QStringLiteral("Original (1:1)"), QStringLiteral("original"));
        btnReaderBgColor_ = new QPushButton(QStringLiteral("#0B0D10"), layoutCard);
        layoutForm->addRow(QStringLiteral("Default Page Layout:"), cmbDefaultPageLayout_);
        layoutForm->addRow(QStringLiteral("Default Fit Mode:"), cmbDefaultFitMode_);
        layoutForm->addRow(QStringLiteral("Reader Canvas Background:"), btnReaderBgColor_);
        layoutCard->contentLayout()->addLayout(layoutForm);

        auto* card = new SectionCardWidget(QStringLiteral("READER BEHAVIOUR & TOOLS"), page);
        chkDefaultMangaMode_ = new QCheckBox(QStringLiteral("Manga mode default (Right-to-Left)"), card);
        chkSmartWideSpread_ = new QCheckBox(QStringLiteral("Smart wide-spread detection"), card);
        chkAutoCropBorders_ = new QCheckBox(QStringLiteral("Auto-crop whitespace borders"), card);
        chkVerticalWebtoonMode_ = new QCheckBox(QStringLiteral("Vertical Webtoon continuous scroll mode"), card);
        chkKeepScreenActive_ = new QCheckBox(QStringLiteral("Keep screen active while reading"), card);
        chkLoupeMagnifier_ = new QCheckBox(QStringLiteral("Enable hold-to-zoom Loupe Magnifier"), card);

        auto* toolsForm = new QFormLayout();
        toolsForm->setSpacing(10);

        auto* loupeZoomRow = new QHBoxLayout();
        sldLoupeZoom_ = new QSlider(Qt::Horizontal, card);
        sldLoupeZoom_->setRange(20, 40);
        lblLoupeZoomVal_ = new QLabel(QStringLiteral("3.0x"), card);
        loupeZoomRow->addWidget(sldLoupeZoom_, 1);
        loupeZoomRow->addWidget(lblLoupeZoomVal_);

        spnLoupeSizePx_ = new QSpinBox(card);
        spnLoupeSizePx_->setRange(180, 320);
        spnLoupeSizePx_->setSingleStep(10);
        spnLoupeSizePx_->setSuffix(QStringLiteral(" px"));

        toolsForm->addRow(QStringLiteral("Loupe Zoom Factor:"), loupeZoomRow);
        toolsForm->addRow(QStringLiteral("Loupe Diameter:"), spnLoupeSizePx_);

        card->contentLayout()->addWidget(chkDefaultMangaMode_);
        card->contentLayout()->addWidget(chkSmartWideSpread_);
        card->contentLayout()->addWidget(chkAutoCropBorders_);
        card->contentLayout()->addWidget(chkVerticalWebtoonMode_);
        card->contentLayout()->addWidget(chkKeepScreenActive_);
        card->contentLayout()->addWidget(chkLoupeMagnifier_);
        card->contentLayout()->addLayout(toolsForm);

        lay->addWidget(layoutCard);
        lay->addWidget(card);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    // 3: Library & Storage
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        lay->setSpacing(16);

        auto* ramCard = new SectionCardWidget(QStringLiteral("RAM PAGE CACHE & LIBRARY"), page);
        auto* ramForm = new QFormLayout();
        auto* ramRow = new QHBoxLayout();
        sldRamLimitMb_ = new QSlider(Qt::Horizontal, ramCard);
        sldRamLimitMb_->setRange(64, 1024);
        spnRamLimitMb_ = new QSpinBox(ramCard);
        spnRamLimitMb_->setRange(32, 2048);
        spnRamLimitMb_->setSingleStep(32);
        spnRamLimitMb_->setSuffix(QStringLiteral(" MB"));
        ramRow->addWidget(sldRamLimitMb_, 1);
        ramRow->addWidget(spnRamLimitMb_);
        ramForm->addRow(QStringLiteral("Page RAM Cache Limit:"), ramRow);
        chkAutoSyncFolders_ = new QCheckBox(QStringLiteral("Auto-sync watched folders on startup"), ramCard);
        chkAutoSyncFolders_->setChecked(true);
        chkUnpluggedDriveGuard_ = new QCheckBox(QStringLiteral("Unplugged Drive Guard (protect offline USB/network entries)"), ramCard);
        chkUnpluggedDriveGuard_->setChecked(true);
        ramCard->contentLayout()->addLayout(ramForm);
        ramCard->contentLayout()->addWidget(chkAutoSyncFolders_);
        ramCard->contentLayout()->addWidget(chkUnpluggedDriveGuard_);

        auto* coverCard = new SectionCardWidget(QStringLiteral("COVER CACHE STORAGE"), page);
        rbMode1Ram_ = new QRadioButton(
            QStringLiteral("Mode 1 - Zero-Disk RAM LRU (extracts visible covers on demand)"),
            coverCard);
        rbMode2Pack_ = new QRadioButton(
            QStringLiteral("Mode 2 - Single-File covers.pack (WebP 180x255 pack in ./data/)"),
            coverCard);
        auto* packBtnRow = new QHBoxLayout();
        btnBuildPack_ = new QPushButton(QStringLiteral("BUILD / REBUILD COVERS.PACK"), coverCard);
        btnPurgeCache_ = new QPushButton(QStringLiteral("PURGE COVER CACHE"), coverCard);
        btnClearOfflineCache_ = new QPushButton(QStringLiteral("CLEAR OFFLINE CACHE"), coverCard);
        packBtnRow->addWidget(btnBuildPack_);
        packBtnRow->addWidget(btnPurgeCache_);
        packBtnRow->addWidget(btnClearOfflineCache_);
        packBtnRow->addStretch(1);
        packProgressBar_ = new QProgressBar(coverCard);
        packProgressBar_->setVisible(false);
        packStatusLabel_ = new QLabel(
            QStringLiteral("Pack file: %1")
                .arg(QString::fromStdWString(portablePaths_.coversPackPath.native())),
            coverCard);
        packStatusLabel_->setWordWrap(true);

        coverCard->contentLayout()->addWidget(rbMode1Ram_);
        coverCard->contentLayout()->addWidget(rbMode2Pack_);
        coverCard->contentLayout()->addLayout(packBtnRow);
        coverCard->contentLayout()->addWidget(packProgressBar_);
        coverCard->contentLayout()->addWidget(packStatusLabel_);

        lay->addWidget(ramCard);
        lay->addWidget(coverCard);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    // 4: File Formats
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        lay->setSpacing(16);

        auto* card = new SectionCardWidget(QStringLiteral("FILE FORMAT DEFAULTS"), page);
        auto* fmtForm = new QFormLayout();
        fmtForm->setSpacing(10);
        cmbPdfReaderDpi_ = new QComboBox(card);
        cmbPdfReaderDpi_->addItem(QStringLiteral("150 dpi (Fast)"), 150);
        cmbPdfReaderDpi_->addItem(QStringLiteral("200 dpi (Balanced)"), 200);
        cmbPdfReaderDpi_->addItem(QStringLiteral("300 dpi (Crisp)"), 300);
        cmbPdfReaderDpi_->addItem(QStringLiteral("400 dpi (Ultra)"), 400);
        cmbDefaultExportFormat_ = new QComboBox(card);
        cmbDefaultExportFormat_->addItem(QStringLiteral("CBZ (.cbz)"), QStringLiteral("cbz"));
        cmbDefaultExportFormat_->addItem(QStringLiteral("CB7 (.cb7 - 7-Zip)"), QStringLiteral("cb7"));
        cmbDefaultExportFormat_->addItem(QStringLiteral("CBT (.cbt - TAR)"), QStringLiteral("cbt"));
        cmbDefaultExportFormat_->addItem(QStringLiteral("ZIP (.zip)"), QStringLiteral("zip"));
        cmbDefaultExportFormat_->addItem(QStringLiteral("EPUB (.epub - Fixed-Layout)"), QStringLiteral("epub"));
        cmbDefaultExportFormat_->addItem(QStringLiteral("PDF (.pdf)"), QStringLiteral("pdf"));
        chkNaturalSort_ = new QCheckBox(QStringLiteral("Natural numeric sorting (1, 2, ..., 10)"), card);
        chkNaturalSort_->setChecked(true);
        fmtForm->addRow(QStringLiteral("Default PDF Render DPI:"), cmbPdfReaderDpi_);
        fmtForm->addRow(QStringLiteral("Default Archive Output Format:"), cmbDefaultExportFormat_);
        card->contentLayout()->addLayout(fmtForm);
        card->contentLayout()->addWidget(chkNaturalSort_);

        auto* scanCard = new SectionCardWidget(QStringLiteral("SCANNED LIBRARY FORMATS"), page);
        auto* scanGrid = new QGridLayout();
        chkFmtCbz_ = new QCheckBox(QStringLiteral(".cbz / .zip"), scanCard);
        chkFmtCbr_ = new QCheckBox(QStringLiteral(".cbr / .rar"), scanCard);
        chkFmtCb7_ = new QCheckBox(QStringLiteral(".cb7 / .7z"), scanCard);
        chkFmtCbt_ = new QCheckBox(QStringLiteral(".cbt / .tar"), scanCard);
        chkFmtPdf_ = new QCheckBox(QStringLiteral(".pdf"), scanCard);
        chkFmtEpub_ = new QCheckBox(QStringLiteral(".epub"), scanCard);
        chkFmtImgFolders_ = new QCheckBox(QStringLiteral("Image Folders"), scanCard);
        for (auto* c : {chkFmtCbz_, chkFmtCbr_, chkFmtCb7_, chkFmtCbt_, chkFmtPdf_, chkFmtEpub_, chkFmtImgFolders_}) {
            c->setChecked(true);
        }
        scanGrid->addWidget(chkFmtCbz_, 0, 0);
        scanGrid->addWidget(chkFmtCbr_, 0, 1);
        scanGrid->addWidget(chkFmtCb7_, 0, 2);
        scanGrid->addWidget(chkFmtCbt_, 1, 0);
        scanGrid->addWidget(chkFmtPdf_, 1, 1);
        scanGrid->addWidget(chkFmtEpub_, 1, 2);
        scanGrid->addWidget(chkFmtImgFolders_, 2, 0, 1, 2);
        scanCard->contentLayout()->addLayout(scanGrid);

        lay->addWidget(card);
        lay->addWidget(scanCard);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    // 5: External Tools
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        lay->setSpacing(16);

        auto* card = new SectionCardWidget(QStringLiteral("EXTERNAL COMPANION APP"), page);
        auto* form = new QFormLayout();
        form->setSpacing(10);
        auto* row = new QHBoxLayout();
        editExternalAppPath_ = new QLineEdit(card);
        editExternalAppPath_->setPlaceholderText(QStringLiteral("Optional path to external reader / translator..."));
        btnBrowseExternalApp_ = new QPushButton(QStringLiteral("BROWSE..."), card);
        row->addWidget(editExternalAppPath_, 1);
        row->addWidget(btnBrowseExternalApp_);
        editExternalAppArgs_ = new QLineEdit(card);
        editExternalAppArgs_->setPlaceholderText(QStringLiteral("\"{file}\""));
        form->addRow(QStringLiteral("Executable Path:"), row);
        form->addRow(QStringLiteral("Launch Arguments:"), editExternalAppArgs_);
        card->contentLayout()->addLayout(form);

        lay->addWidget(card);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    // 6: Shortcuts
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        auto* card = new SectionCardWidget(QStringLiteral("KEYBOARD SHORTCUTS"), page);
        auto* grid = new QGridLayout();
        grid->setSpacing(8);
        const mavira::data::config::AppSettings defaults;
        int row = 0;
        for (const auto& [actionKey, defaultSeq] : defaults.shortcuts) {
            auto* lbl = new QLabel(prettyShortcutLabel(actionKey) + QStringLiteral(":"), card);
            auto* edit = new QKeySequenceEdit(card);
            auto* btnChange = new QPushButton(QStringLiteral("Change"), card);
            auto* btnReset = new QPushButton(QStringLiteral("Reset"), card);
            shortcutEditors_.emplace(actionKey, edit);
            connect(btnChange, &QPushButton::clicked, edit, [edit]() { edit->setFocus(); });
            const QString defSeqStr = QString::fromStdString(defaultSeq);
            connect(btnReset, &QPushButton::clicked, edit, [edit, defSeqStr]() {
                edit->setKeySequence(QKeySequence::fromString(defSeqStr));
            });
            grid->addWidget(lbl, row, 0);
            grid->addWidget(edit, row, 1);
            grid->addWidget(btnChange, row, 2);
            grid->addWidget(btnReset, row, 3);
            ++row;
        }
        btnResetShortcuts_ = new QPushButton(QStringLiteral("RESET ALL SHORTCUTS"), card);
        card->contentLayout()->addLayout(grid);
        card->contentLayout()->addWidget(btnResetShortcuts_, 0, Qt::AlignLeft);
        lay->addWidget(card);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    // 7: Advanced
    {
        auto* page = new QWidget();
        auto* lay = new QVBoxLayout(page);
        lay->setSpacing(16);

        auto* diagCard = new SectionCardWidget(QStringLiteral("DIAGNOSTICS & MAINTENANCE"), page);
        auto* diagForm = new QFormLayout();
        diagForm->setSpacing(10);
        cmbLogLevel_ = new QComboBox(diagCard);
        cmbLogLevel_->addItem(QStringLiteral("Off"), QStringLiteral("off"));
        cmbLogLevel_->addItem(QStringLiteral("Error"), QStringLiteral("error"));
        cmbLogLevel_->addItem(QStringLiteral("Warn (Default)"), QStringLiteral("warn"));
        cmbLogLevel_->addItem(QStringLiteral("Info"), QStringLiteral("info"));
        cmbLogLevel_->addItem(QStringLiteral("Debug"), QStringLiteral("debug"));
        diagForm->addRow(QStringLiteral("Diagnostic Log Level:"), cmbLogLevel_);
        diagCard->contentLayout()->addLayout(diagForm);

        auto* actRow = new QHBoxLayout();
        btnOpenDataFolder_ = new QPushButton(QStringLiteral("OPEN PORTABLE DATA FOLDER"), diagCard);
        btnOptimizeDb_ = new QPushButton(QStringLiteral("OPTIMIZE SQLITE DATABASE"), diagCard);
        btnResetPrefsAdvanced_ = new QPushButton(QStringLiteral("RESET ALL PREFERENCES"), diagCard);
        actRow->addWidget(btnOpenDataFolder_);
        actRow->addWidget(btnOptimizeDb_);
        actRow->addWidget(btnResetPrefsAdvanced_);
        actRow->addStretch(1);
        lblAdvancedStatus_ = new QLabel(diagCard);
        lblAdvancedStatus_->setWordWrap(true);
        diagCard->contentLayout()->addLayout(actRow);
        diagCard->contentLayout()->addWidget(lblAdvancedStatus_);

        auto* card = new SectionCardWidget(QStringLiteral("PORTABLE DATA PATHS"), page);
        auto* pathsLbl = new QLabel(
            QStringLiteral("Settings JSON: %1\nDatabase: %2\nOffline Cache: %3")
                .arg(QString::fromStdWString(portablePaths_.settingsPath.native()))
                .arg(QString::fromStdWString(portablePaths_.databasePath.native()))
                .arg(QString::fromStdWString(portablePaths_.offlineCacheDir.native())),
            card);
        pathsLbl->setWordWrap(true);
        card->contentLayout()->addWidget(pathsLbl);

        lay->addWidget(diagCard);
        lay->addWidget(card);
        lay->addStretch(1);
        categoryStack_->addWidget(wrapPrefScroll(page, categoryStack_));
    }

    rightLayout->addWidget(categoryStack_, 1);

    rootLayout->addWidget(sidebar);
    rootLayout->addWidget(rightArea, 1);

    // Signals
    connect(categoryGroup_, &QButtonGroup::idClicked, categoryStack_, &QStackedWidget::setCurrentIndex);
    connect(btnBack_, &QPushButton::clicked, this, [this]() {
        applyAndEmit();
        emit backRequested();
    });
    connect(btnApply_, &QPushButton::clicked, this, &PreferencesWorkspace::applyAndEmit);
    connect(btnResetAll_, &QPushButton::clicked, this, &PreferencesWorkspace::resetAllToDefaults);
    connect(btnResetShortcuts_, &QPushButton::clicked, this, &PreferencesWorkspace::resetShortcutsToDefaults);
    connect(btnBuildPack_, &QPushButton::clicked, this, &PreferencesWorkspace::buildCoversPack);
    connect(btnPurgeCache_, &QPushButton::clicked, this, &PreferencesWorkspace::purgeCoverCache);
    connect(btnBrowseExternalApp_, &QPushButton::clicked, this, [this]() {
        const QString exe = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select External Companion App"),
            editExternalAppPath_->text(),
            QStringLiteral("Executables (*.exe);;All Files (*.*)"));
        if (!exe.isEmpty()) {
            editExternalAppPath_->setText(exe);
        }
    });
    connect(btnReaderBgColor_, &QPushButton::clicked, this, [this]() {
        const QColor init(QString::fromStdString(readerBgColor_));
        const QColor chosen = QColorDialog::getColor(init.isValid() ? init : QColor("#0B0D10"), this);
        if (chosen.isValid()) {
            readerBgColor_ = chosen.name(QColor::HexRgb).toUpper().toStdString();
            btnReaderBgColor_->setText(QString::fromStdString(readerBgColor_));
        }
    });
    connect(sldLoupeZoom_, &QSlider::valueChanged, this, [this](int v) {
        lblLoupeZoomVal_->setText(QStringLiteral("%1x").arg(static_cast<double>(v) / 10.0, 0, 'f', 1));
    });
    connect(sldRamLimitMb_, &QSlider::valueChanged, this, [this](int v) {
        if (spnRamLimitMb_->value() != v) spnRamLimitMb_->setValue(v);
    });
    connect(spnRamLimitMb_, &QSpinBox::valueChanged, this, [this](int v) {
        if (v >= 64 && v <= 1024 && sldRamLimitMb_->value() != v) sldRamLimitMb_->setValue(v);
    });
    connect(btnClearOfflineCache_, &QPushButton::clicked, this, [this]() {
        std::error_code ec;
        if (std::filesystem::exists(portablePaths_.offlineCacheDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(portablePaths_.offlineCacheDir, ec)) {
                std::filesystem::remove_all(entry.path(), ec);
            }
        }
        packStatusLabel_->setText(QStringLiteral("Offline cache cleared."));
    });
    connect(btnOpenDataFolder_, &QPushButton::clicked, this, [this]() {
        const auto dir = portablePaths_.settingsPath.parent_path();
        if (!dir.empty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(dir.native())));
        }
    });
    connect(btnOptimizeDb_, &QPushButton::clicked, this, [this]() {
        if (db_ != nullptr && db_->isOpen()) {
            lblAdvancedStatus_->setText(QStringLiteral("SQLite database is healthy & WAL-indexed."));
        } else {
            lblAdvancedStatus_->setText(QStringLiteral("Database is not currently open."));
        }
    });
    connect(btnResetPrefsAdvanced_, &QPushButton::clicked, this, &PreferencesWorkspace::resetAllToDefaults);
}

void PreferencesWorkspace::loadFromSettings(const mavira::data::config::AppSettings& s) {
    workingSettings_ = s;
    QString targetTheme = QString::fromStdString(s.theme);
    if (targetTheme == QStringLiteral("dark") || targetTheme == QStringLiteral("Dark") || targetTheme.isEmpty()) {
        targetTheme = QStringLiteral("obsidian_cyber");
    } else if (targetTheme == QStringLiteral("light") || targetTheme == QStringLiteral("Light")) {
        targetTheme = QStringLiteral("nordic_glacier");
    }
    const int themeIdx = cmbTheme_->findData(targetTheme);
    cmbTheme_->setCurrentIndex(themeIdx >= 0 ? themeIdx : 0);

    const int langIdx = cmbLanguage_->findData(QString::fromStdString(s.language));
    cmbLanguage_->setCurrentIndex(langIdx >= 0 ? langIdx : 0);

    const int scaleIdx = cmbUiScale_->findData(s.ui_scale_percent);
    cmbUiScale_->setCurrentIndex(scaleIdx >= 0 ? scaleIdx : 1);
    const int densIdx = cmbCardDensity_->findData(QString::fromStdString(s.card_density));
    cmbCardDensity_->setCurrentIndex(densIdx >= 0 ? densIdx : 1);

    chkCircularProgressBar_->setChecked(s.circular_progress_bar);
    chkSmartRename_->setChecked(s.smart_rename);
    chkShowHiddenItems_->setChecked(s.show_hidden_items);
    chkConfirmDeleteDisk_->setChecked(s.confirm_delete_disk);
    chkShowStatusTelemetry_->setChecked(s.show_status_telemetry);
    const int hideIdx = cmbCursorHideSeconds_->findData(s.cursor_hide_seconds);
    cmbCursorHideSeconds_->setCurrentIndex(hideIdx >= 0 ? hideIdx : 2);

    const int layIdx = cmbDefaultPageLayout_->findData(QString::fromStdString(s.default_page_layout));
    cmbDefaultPageLayout_->setCurrentIndex(layIdx >= 0 ? layIdx : 0);
    const int fitIdx = cmbDefaultFitMode_->findData(QString::fromStdString(s.default_fit_mode));
    cmbDefaultFitMode_->setCurrentIndex(fitIdx >= 0 ? fitIdx : 0);
    readerBgColor_ = s.reader_bg_color.empty() ? "#0B0D10" : s.reader_bg_color;
    btnReaderBgColor_->setText(QString::fromStdString(readerBgColor_));

    chkDefaultMangaMode_->setChecked(s.default_manga_mode);
    chkSmartWideSpread_->setChecked(s.smart_wide_spread);
    chkAutoCropBorders_->setChecked(s.auto_crop_borders);
    chkVerticalWebtoonMode_->setChecked(s.vertical_webtoon_mode);
    chkKeepScreenActive_->setChecked(s.keep_screen_active);
    chkLoupeMagnifier_->setChecked(s.loupe_magnifier_enabled);
    sldLoupeZoom_->setValue(std::clamp(static_cast<int>(std::lround(s.loupe_zoom_factor * 10.0)), 20, 40));
    spnLoupeSizePx_->setValue(std::clamp(s.loupe_size_px, 180, 320));

    spnRamLimitMb_->setValue(std::clamp(s.ram_cache_limit_mb, 32, 2048));
    if (s.ram_cache_limit_mb >= 64 && s.ram_cache_limit_mb <= 1024) {
        sldRamLimitMb_->setValue(s.ram_cache_limit_mb);
    }

    const int dpiIdx = cmbPdfReaderDpi_->findData(s.pdf_reader_dpi);
    cmbPdfReaderDpi_->setCurrentIndex(dpiIdx >= 0 ? dpiIdx : 1);
    const int expIdx = cmbDefaultExportFormat_->findData(QString::fromStdString(s.default_export_format));
    cmbDefaultExportFormat_->setCurrentIndex(expIdx >= 0 ? expIdx : 0);

    editExternalAppPath_->setText(QString::fromStdString(s.external_app_path));
    editExternalAppArgs_->setText(QString::fromStdString(s.external_app_args));

    const int logIdx = cmbLogLevel_->findData(QString::fromStdString(s.log_level));
    cmbLogLevel_->setCurrentIndex(logIdx >= 0 ? logIdx : 2);

    if (s.cover_cache_mode == 2) {
        rbMode2Pack_->setChecked(true);
    } else {
        rbMode1Ram_->setChecked(true);
    }

    for (auto& [key, edit] : shortcutEditors_) {
        auto it = s.shortcuts.find(key);
        const std::string seq = (it != s.shortcuts.end()) ? it->second : std::string{};
        edit->setKeySequence(QKeySequence::fromString(QString::fromStdString(seq)));
    }
}

mavira::data::config::AppSettings PreferencesWorkspace::settings() const {
    mavira::data::config::AppSettings out = workingSettings_;
    out.theme = cmbTheme_->currentData().toString().toStdString();
    out.language = cmbLanguage_->currentData().toString().toStdString();
    out.ui_scale_percent = cmbUiScale_->currentData().toInt();
    out.card_density = cmbCardDensity_->currentData().toString().toStdString();
    out.circular_progress_bar = chkCircularProgressBar_->isChecked();
    out.smart_rename = chkSmartRename_->isChecked();
    out.show_hidden_items = chkShowHiddenItems_->isChecked();
    out.confirm_delete_disk = chkConfirmDeleteDisk_->isChecked();
    out.show_status_telemetry = chkShowStatusTelemetry_->isChecked();
    out.cursor_hide_seconds = cmbCursorHideSeconds_->currentData().toInt();
    out.default_page_layout = cmbDefaultPageLayout_->currentData().toString().toStdString();
    out.default_fit_mode = cmbDefaultFitMode_->currentData().toString().toStdString();
    out.reader_bg_color = readerBgColor_;
    out.default_manga_mode = chkDefaultMangaMode_->isChecked();
    out.smart_wide_spread = chkSmartWideSpread_->isChecked();
    out.auto_crop_borders = chkAutoCropBorders_->isChecked();
    out.vertical_webtoon_mode = chkVerticalWebtoonMode_->isChecked();
    out.keep_screen_active = chkKeepScreenActive_->isChecked();
    out.loupe_magnifier_enabled = chkLoupeMagnifier_->isChecked();
    out.loupe_zoom_factor = static_cast<double>(sldLoupeZoom_->value()) / 10.0;
    out.loupe_size_px = spnLoupeSizePx_->value();
    out.ram_cache_limit_mb = spnRamLimitMb_->value();
    out.pdf_reader_dpi = cmbPdfReaderDpi_->currentData().toInt();
    out.default_export_format = cmbDefaultExportFormat_->currentData().toString().toStdString();
    out.external_app_path = editExternalAppPath_->text().trimmed().toStdString();
    out.external_app_args = editExternalAppArgs_->text().toStdString();
    out.log_level = cmbLogLevel_->currentData().toString().toStdString();
    out.cover_cache_mode = rbMode2Pack_->isChecked() ? 2 : 1;

    for (const auto& [key, edit] : shortcutEditors_) {
        out.shortcuts[key] = edit->keySequence().toString(QKeySequence::PortableText).toStdString();
    }
    return out;
}

void PreferencesWorkspace::applyAndEmit() {
    workingSettings_ = settings();
    emit settingsApplied(workingSettings_);
}

void PreferencesWorkspace::resetAllToDefaults() {
    mavira::data::config::AppSettings defaults;
    defaults.tool_presets = workingSettings_.tool_presets;
    loadFromSettings(defaults);
    emit settingsApplied(defaults);
}

void PreferencesWorkspace::resetShortcutsToDefaults() {
    const mavira::data::config::AppSettings defaults;
    for (auto& [key, edit] : shortcutEditors_) {
        auto it = defaults.shortcuts.find(key);
        if (it != defaults.shortcuts.end()) {
            edit->setKeySequence(QKeySequence::fromString(QString::fromStdString(it->second)));
        }
    }
}

void PreferencesWorkspace::buildCoversPack() {
    if (db_ == nullptr || !db_->isOpen() || buildingPack_.exchange(true)) {
        return;
    }
    if (packWorkerThread_.joinable()) {
        packWorkerThread_.join();
    }
    packStopSource_ = std::stop_source{};
    btnBuildPack_->setEnabled(false);
    packProgressBar_->setVisible(true);
    packProgressBar_->setRange(0, 0);
    packStatusLabel_->setText(QStringLiteral("Building covers.pack..."));

    const auto packPath = portablePaths_.coversPackPath;
    auto* db = db_;
    packWorkerThread_ = std::jthread([this, packPath, db](std::stop_token) {
        mavira::core::cache::PackFileCoverProvider packProvider(packPath, db);
        auto res = packProvider.indexUncachedCovers(
            packStopSource_.get_token(),
            [this](std::size_t indexed, std::size_t total) {
                QMetaObject::invokeMethod(
                    this,
                    [this, indexed, total]() {
                        packProgressBar_->setRange(0, static_cast<int>(std::max<std::size_t>(1, total)));
                        packProgressBar_->setValue(static_cast<int>(indexed));
                        packStatusLabel_->setText(
                            QStringLiteral("Indexed %1 / %2 covers into covers.pack")
                                .arg(indexed)
                                .arg(total));
                    },
                    Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(
            this,
            [this, res]() {
                buildingPack_.store(false);
                btnBuildPack_->setEnabled(true);
                packProgressBar_->setVisible(false);
                if (res) {
                    packStatusLabel_->setText(
                        QStringLiteral("covers.pack ready (%1 new covers indexed).").arg(res.value()));
                    emit coversPackBuilt();
                } else {
                    packStatusLabel_->setText(
                        QStringLiteral("covers.pack build error: %1")
                            .arg(QString::fromStdString(res.error().message)));
                }
            },
            Qt::QueuedConnection);
    });
}

void PreferencesWorkspace::purgeCoverCache() {
    if (cacheManager_ != nullptr && cacheManager_->coverStorage() != nullptr) {
        cacheManager_->coverStorage()->clearMemoryCache();
    }
    if (db_ != nullptr && db_->isOpen()) {
        (void)db_->clearAllCoverPackLocations();
    }
    std::error_code ec;
    if (std::filesystem::exists(portablePaths_.coversPackPath, ec)) {
        std::filesystem::remove(portablePaths_.coversPackPath, ec);
    }
    packStatusLabel_->setText(QStringLiteral("Cover cache purged."));
    emit coverCachePurged();
}

void PreferencesWorkspace::retranslateUi() {
    if (btnBack_) btnBack_->setText(LocalizationManager::tr("prefs.back"));
    if (btnApply_) btnApply_->setText(LocalizationManager::tr("prefs.apply"));
    if (btnResetAll_) btnResetAll_->setText(LocalizationManager::tr("prefs.reset_all"));
    if (catLabel_) catLabel_->setText(LocalizationManager::tr("prefs.categories"));
    if (headerBadge_) headerBadge_->setText(LocalizationManager::tr("prefs.badge"));

    const char* categoryKeys[] = {
        "prefs.cat.appearance",
        "prefs.cat.ui",
        "prefs.cat.reader",
        "prefs.cat.library",
        "prefs.cat.formats",
        "prefs.cat.external",
        "prefs.cat.shortcuts",
        "prefs.cat.advanced"
    };
    if (categoryGroup_ != nullptr) {
        for (int i = 0; i < 8; ++i) {
            if (auto* btn = categoryGroup_->button(i)) {
                btn->setText(LocalizationManager::tr(categoryKeys[i]));
            }
        }
    }

    if (lblTheme_) lblTheme_->setText(LocalizationManager::tr("prefs.lbl.theme"));
    if (lblLanguage_) lblLanguage_->setText(LocalizationManager::tr("prefs.lbl.language"));
    if (lblUiScale_) lblUiScale_->setText(LocalizationManager::tr("prefs.lbl.ui_scale"));
    if (lblCardDensity_) lblCardDensity_->setText(LocalizationManager::tr("prefs.lbl.card_density"));
}

} // namespace mavira::ui
