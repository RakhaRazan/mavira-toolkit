#pragma once

#include "core/cache/CacheManager.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

#include <QString>
#include <QWidget>

#include <atomic>
#include <map>
#include <stop_token>
#include <thread>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QStackedWidget;

namespace mavira::ui {

class SectionCardWidget;

/// Full-area Preferences workspace inside `MainWindow`'s `QStackedWidget`
/// with a 240px left category sidebar and `SectionCardWidget` cards on the right.
class PreferencesWorkspace : public QWidget {
    Q_OBJECT

public:
    PreferencesWorkspace(
        mavira::data::db::DatabaseManager* db,
        mavira::core::cache::CacheManager* cacheManager,
        const mavira::data::fs::PortablePaths& portablePaths,
        QWidget* parent = nullptr);
    ~PreferencesWorkspace() override;

    void setPortablePaths(const mavira::data::fs::PortablePaths& paths) noexcept {
        portablePaths_ = paths;
    }

    void loadFromSettings(const mavira::data::config::AppSettings& settings);
    [[nodiscard]] mavira::data::config::AppSettings settings() const;
    void retranslateUi();

    // Accessors for testing
    [[nodiscard]] QStackedWidget* categoryStack() const noexcept { return categoryStack_; }
    [[nodiscard]] QPushButton* backButton() const noexcept { return btnBack_; }
    [[nodiscard]] QPushButton* resetAllButton() const noexcept { return btnResetAll_; }

public slots:
    void applyAndEmit();
    void resetAllToDefaults();
    void resetShortcutsToDefaults();
    void buildCoversPack();
    void purgeCoverCache();

signals:
    void backRequested();
    void settingsApplied(const mavira::data::config::AppSettings& settings);
    void coverCachePurged();
    void coversPackBuilt();

private:
    void setupUi();

    mavira::data::config::AppSettings workingSettings_{};
    mavira::data::db::DatabaseManager* db_{nullptr};
    mavira::core::cache::CacheManager* cacheManager_{nullptr};
    mavira::data::fs::PortablePaths portablePaths_{};

    // Left Sidebar
    QPushButton* btnBack_{nullptr};
    QPushButton* btnApply_{nullptr};
    QPushButton* btnResetAll_{nullptr};
    QLabel* catLabel_{nullptr};
    QLabel* headerBadge_{nullptr};
    QButtonGroup* categoryGroup_{nullptr};
    QStackedWidget* categoryStack_{nullptr};

    // 1. Appearance
    QLabel* lblTheme_{nullptr};
    QLabel* lblLanguage_{nullptr};
    QLabel* lblUiScale_{nullptr};
    QLabel* lblCardDensity_{nullptr};
    QComboBox* cmbTheme_{nullptr};
    QComboBox* cmbLanguage_{nullptr};
    QComboBox* cmbUiScale_{nullptr};
    QComboBox* cmbCardDensity_{nullptr};

    // 2. User Interface
    QCheckBox* chkCircularProgressBar_{nullptr};
    QCheckBox* chkSmartRename_{nullptr};
    QCheckBox* chkShowHiddenItems_{nullptr};
    QCheckBox* chkConfirmDeleteDisk_{nullptr};
    QCheckBox* chkShowStatusTelemetry_{nullptr};
    QComboBox* cmbCursorHideSeconds_{nullptr};

    // 3. Reader
    QComboBox* cmbDefaultPageLayout_{nullptr};
    QComboBox* cmbDefaultFitMode_{nullptr};
    QPushButton* btnReaderBgColor_{nullptr};
    std::string readerBgColor_{"#0B0D10"};
    QCheckBox* chkDefaultMangaMode_{nullptr};
    QCheckBox* chkSmartWideSpread_{nullptr};
    QCheckBox* chkAutoCropBorders_{nullptr};
    QCheckBox* chkVerticalWebtoonMode_{nullptr};
    QCheckBox* chkKeepScreenActive_{nullptr};
    QCheckBox* chkLoupeMagnifier_{nullptr};
    QSlider* sldLoupeZoom_{nullptr};
    QLabel* lblLoupeZoomVal_{nullptr};
    QSpinBox* spnLoupeSizePx_{nullptr};

    // 4. Library & Storage
    QSlider* sldRamLimitMb_{nullptr};
    QSpinBox* spnRamLimitMb_{nullptr};
    QRadioButton* rbMode1Ram_{nullptr};
    QRadioButton* rbMode2Pack_{nullptr};
    QPushButton* btnBuildPack_{nullptr};
    QPushButton* btnPurgeCache_{nullptr};
    QPushButton* btnClearOfflineCache_{nullptr};
    QCheckBox* chkAutoSyncFolders_{nullptr};
    QCheckBox* chkUnpluggedDriveGuard_{nullptr};
    QProgressBar* packProgressBar_{nullptr};
    QLabel* packStatusLabel_{nullptr};

    // 5. File Formats
    QComboBox* cmbPdfReaderDpi_{nullptr};
    QComboBox* cmbDefaultExportFormat_{nullptr};
    QCheckBox* chkNaturalSort_{nullptr};
    QCheckBox* chkFmtCbz_{nullptr};
    QCheckBox* chkFmtCbr_{nullptr};
    QCheckBox* chkFmtCb7_{nullptr};
    QCheckBox* chkFmtCbt_{nullptr};
    QCheckBox* chkFmtPdf_{nullptr};
    QCheckBox* chkFmtEpub_{nullptr};
    QCheckBox* chkFmtImgFolders_{nullptr};

    // 6. External Tools
    QLineEdit* editExternalAppPath_{nullptr};
    QPushButton* btnBrowseExternalApp_{nullptr};
    QLineEdit* editExternalAppArgs_{nullptr};

    // 7. Shortcuts
    std::map<std::string, QKeySequenceEdit*> shortcutEditors_{};
    QPushButton* btnResetShortcuts_{nullptr};

    // 8. Advanced
    QComboBox* cmbLogLevel_{nullptr};
    QPushButton* btnOpenDataFolder_{nullptr};
    QPushButton* btnOptimizeDb_{nullptr};
    QPushButton* btnResetPrefsAdvanced_{nullptr};
    QLabel* lblAdvancedStatus_{nullptr};

    struct LocalizedCard {
        SectionCardWidget* card{nullptr};
        const char* key{nullptr};
    };
    std::vector<LocalizedCard> sectionCards_{};

    std::stop_source packStopSource_{};
    std::jthread packWorkerThread_{};
    std::atomic<bool> buildingPack_{false};
};

} // namespace mavira::ui
