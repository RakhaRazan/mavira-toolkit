#include "ui/SettingsDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace mavira::ui {

namespace {

[[nodiscard]] QString prettyShortcutName(const std::string& key) {
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

SettingsDialog::SettingsDialog(
    mavira::data::config::AppSettings currentSettings,
    mavira::data::db::DatabaseManager* db,
    mavira::core::cache::CacheManager* cacheManager,
    const mavira::data::fs::PortablePaths& portablePaths,
    QWidget* parent)
    : QDialog(parent),
      workingSettings_(std::move(currentSettings)),
      db_(db),
      cacheManager_(cacheManager),
      portablePaths_(portablePaths) {
    setWindowTitle(QStringLiteral("MAVIRA Settings"));
    setModal(true);
    resize(560, 520);

    setupUi();
    loadFromSettings(workingSettings_);
}

SettingsDialog::~SettingsDialog() {
    packStopSource_.request_stop();
    if (packWorkerThread_.joinable()) {
        packWorkerThread_.join();
    }
}

void SettingsDialog::setupUi() {
    auto* rootLayout = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);

    // -------------------------------------------------------------------------
    // Tab 1: General & Library
    // -------------------------------------------------------------------------
    auto* generalTab = new QWidget(tabs_);
    auto* genLayout = new QVBoxLayout(generalTab);

    auto* libraryGroup = new QGroupBox(QStringLiteral("Library & Appearance"), generalTab);
    auto* libLayout = new QVBoxLayout(libraryGroup);

    chkCircularProgressBar_ = new QCheckBox(QStringLiteral("Show circular progress bar on covers"), libraryGroup);
    chkSmartRename_ = new QCheckBox(QStringLiteral("Smart Rename (clean release tags in library titles)"), libraryGroup);
    chkShowHiddenItems_ = new QCheckBox(QStringLiteral("Show hidden items"), libraryGroup);

    auto* themeRow = new QHBoxLayout();
    themeRow->addWidget(new QLabel(QStringLiteral("Theme:"), libraryGroup));
    cmbTheme_ = new QComboBox(libraryGroup);
    cmbTheme_->addItem(QStringLiteral("Obsidian Cyber (Dark)"), QStringLiteral("obsidian_cyber"));
    cmbTheme_->addItem(QStringLiteral("Midnight Velvet (Dark)"), QStringLiteral("midnight_velvet"));
    cmbTheme_->addItem(QStringLiteral("Warm Parchment (Warm Light)"), QStringLiteral("warm_parchment"));
    cmbTheme_->addItem(QStringLiteral("Nordic Glacier (Cold Light)"), QStringLiteral("nordic_glacier"));
    cmbTheme_->addItem(QStringLiteral("Sakura Mist (Soft Light)"), QStringLiteral("sakura_mist"));
    themeRow->addWidget(cmbTheme_);
    themeRow->addStretch(1);

    auto* ramRow = new QHBoxLayout();
    ramRow->addWidget(new QLabel(QStringLiteral("Page RAM Cache Limit (MB):"), libraryGroup));
    spnRamLimitMb_ = new QSpinBox(libraryGroup);
    spnRamLimitMb_->setRange(32, 2048);
    spnRamLimitMb_->setSingleStep(32);
    ramRow->addWidget(spnRamLimitMb_);
    ramRow->addStretch(1);

    auto* extAppRow = new QHBoxLayout();
    extAppRow->addWidget(new QLabel(QStringLiteral("External Companion App:"), libraryGroup));
    editExternalAppPath_ = new QLineEdit(libraryGroup);
    editExternalAppPath_->setPlaceholderText(QStringLiteral("Optional path to Vellum / external viewer..."));
    btnBrowseExternalApp_ = new QPushButton(QStringLiteral("Browse..."), libraryGroup);
    extAppRow->addWidget(editExternalAppPath_, 1);
    extAppRow->addWidget(btnBrowseExternalApp_);

    libLayout->addWidget(chkCircularProgressBar_);
    libLayout->addWidget(chkSmartRename_);
    libLayout->addWidget(chkShowHiddenItems_);
    libLayout->addLayout(themeRow);
    libLayout->addLayout(ramRow);
    libLayout->addLayout(extAppRow);

    auto* readerGroup = new QGroupBox(QStringLiteral("Reader Defaults"), generalTab);
    auto* readerLayout = new QVBoxLayout(readerGroup);

    chkDefaultMangaMode_ = new QCheckBox(QStringLiteral("Manga mode default (Right-to-Left)"), readerGroup);
    chkSmartWideSpread_ = new QCheckBox(QStringLiteral("Smart wide-spread detection"), readerGroup);
    chkAutoCropBorders_ = new QCheckBox(QStringLiteral("Auto-crop whitespace borders"), readerGroup);
    chkVerticalWebtoonMode_ = new QCheckBox(QStringLiteral("Vertical Webtoon continuous scroll mode"), readerGroup);
    chkKeepScreenActive_ = new QCheckBox(QStringLiteral("Keep screen active while reading"), readerGroup);
    chkLoupeMagnifier_ = new QCheckBox(QStringLiteral("Enable hold-to-zoom Loupe Magnifier"), readerGroup);

    readerLayout->addWidget(chkDefaultMangaMode_);
    readerLayout->addWidget(chkSmartWideSpread_);
    readerLayout->addWidget(chkAutoCropBorders_);
    readerLayout->addWidget(chkVerticalWebtoonMode_);
    readerLayout->addWidget(chkKeepScreenActive_);
    readerLayout->addWidget(chkLoupeMagnifier_);

    genLayout->addWidget(libraryGroup);
    genLayout->addWidget(readerGroup);
    genLayout->addStretch(1);
    tabs_->addTab(generalTab, QStringLiteral("General && Library"));

    // -------------------------------------------------------------------------
    // Tab 2: Cover Storage
    // -------------------------------------------------------------------------
    auto* coverTab = new QWidget(tabs_);
    auto* coverLayout = new QVBoxLayout(coverTab);

    auto* modeGroup = new QGroupBox(QStringLiteral("Cover Cache Mode"), coverTab);
    auto* modeLayout = new QVBoxLayout(modeGroup);

    rbMode1Ram_ = new QRadioButton(
        QStringLiteral("Mode 1 - 64 MB RAM LRU Only (0 MB disk bloat, extracts visible covers on demand)"),
        modeGroup);
    rbMode2Pack_ = new QRadioButton(
        QStringLiteral("Mode 2 - Single-File covers.pack (WebP 180x255 pack in ./data/ for instant cold boot)"),
        modeGroup);

    modeLayout->addWidget(rbMode1Ram_);
    modeLayout->addWidget(rbMode2Pack_);

    auto* packGroup = new QGroupBox(QStringLiteral("covers.pack Maintenance"), coverTab);
    auto* packLayout = new QVBoxLayout(packGroup);

    auto* packBtnRow = new QHBoxLayout();
    btnBuildPack_ = new QPushButton(QStringLiteral("Build / Update covers.pack"), packGroup);
    btnPurgeCache_ = new QPushButton(QStringLiteral("Purge Cover Cache"), packGroup);
    packBtnRow->addWidget(btnBuildPack_);
    packBtnRow->addWidget(btnPurgeCache_);
    packBtnRow->addStretch(1);

    packProgressBar_ = new QProgressBar(packGroup);
    packProgressBar_->setVisible(false);

    packStatusLabel_ = new QLabel(
        QStringLiteral("Pack file: %1")
            .arg(QString::fromStdWString(portablePaths_.coversPackPath.native())),
        packGroup);
    packStatusLabel_->setWordWrap(true);

    packLayout->addLayout(packBtnRow);
    packLayout->addWidget(packProgressBar_);
    packLayout->addWidget(packStatusLabel_);

    coverLayout->addWidget(modeGroup);
    coverLayout->addWidget(packGroup);
    coverLayout->addStretch(1);
    tabs_->addTab(coverTab, QStringLiteral("Cover Storage"));

    // -------------------------------------------------------------------------
    // Tab 3: Keyboard Shortcuts
    // -------------------------------------------------------------------------
    auto* shortcutsTab = new QWidget(tabs_);
    auto* scLayout = new QVBoxLayout(shortcutsTab);
    auto* scForm = new QFormLayout();

    const mavira::data::config::AppSettings defaults;
    for (const auto& [actionKey, defaultSeq] : defaults.shortcuts) {
        auto* edit = new QKeySequenceEdit(shortcutsTab);
        shortcutEditors_.emplace(actionKey, edit);
        scForm->addRow(prettyShortcutName(actionKey) + QStringLiteral(":"), edit);
    }

    btnResetShortcuts_ = new QPushButton(QStringLiteral("Reset Shortcuts to Defaults"), shortcutsTab);
    scLayout->addLayout(scForm);
    scLayout->addWidget(btnResetShortcuts_, 0, Qt::AlignLeft);
    scLayout->addStretch(1);
    tabs_->addTab(shortcutsTab, QStringLiteral("Keyboard Shortcuts"));

    rootLayout->addWidget(tabs_, 1);

    // -------------------------------------------------------------------------
    // Dialog Buttons
    // -------------------------------------------------------------------------
    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    rootLayout->addWidget(buttonBox);

    connect(btnBuildPack_, &QPushButton::clicked, this, &SettingsDialog::buildCoversPack);
    connect(btnPurgeCache_, &QPushButton::clicked, this, &SettingsDialog::purgeCoverCache);
    connect(btnResetShortcuts_, &QPushButton::clicked, this, &SettingsDialog::resetShortcutsToDefaults);
    connect(btnBrowseExternalApp_, &QPushButton::clicked, this, [this]() {
        const QString exe = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select External Companion App (e.g. Vellum)"),
            editExternalAppPath_->text(),
            QStringLiteral("Executables (*.exe);;All Files (*.*)"));
        if (!exe.isEmpty()) {
            editExternalAppPath_->setText(exe);
        }
    });

    connect(buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this]() {
        emit settingsApplied(settings());
    });
    connect(buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        emit settingsApplied(settings());
        accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SettingsDialog::loadFromSettings(const mavira::data::config::AppSettings& s) {
    chkDefaultMangaMode_->setChecked(s.default_manga_mode);
    chkSmartWideSpread_->setChecked(s.smart_wide_spread);
    chkAutoCropBorders_->setChecked(s.auto_crop_borders);
    chkVerticalWebtoonMode_->setChecked(s.vertical_webtoon_mode);
    chkKeepScreenActive_->setChecked(s.keep_screen_active);
    chkCircularProgressBar_->setChecked(s.circular_progress_bar);
    chkSmartRename_->setChecked(s.smart_rename);
    chkShowHiddenItems_->setChecked(s.show_hidden_items);
    chkLoupeMagnifier_->setChecked(s.loupe_magnifier_enabled);

    QString targetTheme = QString::fromStdString(s.theme);
    if (targetTheme == QStringLiteral("dark") || targetTheme == QStringLiteral("Dark") || targetTheme.isEmpty()) {
        targetTheme = QStringLiteral("obsidian_cyber");
    } else if (targetTheme == QStringLiteral("light") || targetTheme == QStringLiteral("Light")) {
        targetTheme = QStringLiteral("nordic_glacier");
    }
    const int themeIdx = cmbTheme_->findData(targetTheme);
    cmbTheme_->setCurrentIndex(themeIdx >= 0 ? themeIdx : 0);
    spnRamLimitMb_->setValue(std::clamp(s.ram_cache_limit_mb, 32, 2048));
    editExternalAppPath_->setText(QString::fromStdString(s.external_app_path));

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

mavira::data::config::AppSettings SettingsDialog::settings() const {
    mavira::data::config::AppSettings out = workingSettings_;
    out.default_manga_mode = chkDefaultMangaMode_->isChecked();
    out.smart_wide_spread = chkSmartWideSpread_->isChecked();
    out.auto_crop_borders = chkAutoCropBorders_->isChecked();
    out.vertical_webtoon_mode = chkVerticalWebtoonMode_->isChecked();
    out.keep_screen_active = chkKeepScreenActive_->isChecked();
    out.circular_progress_bar = chkCircularProgressBar_->isChecked();
    out.smart_rename = chkSmartRename_->isChecked();
    out.show_hidden_items = chkShowHiddenItems_->isChecked();
    out.loupe_magnifier_enabled = chkLoupeMagnifier_->isChecked();
    out.theme = cmbTheme_->currentData().toString().toStdString();
    out.ram_cache_limit_mb = spnRamLimitMb_->value();
    out.external_app_path = editExternalAppPath_->text().trimmed().toStdString();
    out.cover_cache_mode = rbMode2Pack_->isChecked() ? 2 : 1;

    for (const auto& [key, edit] : shortcutEditors_) {
        out.shortcuts[key] = edit->keySequence().toString(QKeySequence::PortableText).toStdString();
    }
    return out;
}

void SettingsDialog::resetShortcutsToDefaults() {
    const mavira::data::config::AppSettings defaults;
    for (auto& [key, edit] : shortcutEditors_) {
        auto it = defaults.shortcuts.find(key);
        if (it != defaults.shortcuts.end()) {
            edit->setKeySequence(QKeySequence::fromString(QString::fromStdString(it->second)));
        }
    }
}

void SettingsDialog::buildCoversPack() {
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
                        QStringLiteral("covers.pack ready (%1 new covers indexed).")
                            .arg(res.value()));
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

void SettingsDialog::purgeCoverCache() {
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

} // namespace mavira::ui
