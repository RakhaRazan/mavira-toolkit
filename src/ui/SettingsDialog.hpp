#pragma once

#include "core/cache/CacheManager.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"

#include <QDialog>
#include <QString>

#include <atomic>
#include <map>
#include <stop_token>
#include <thread>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;

namespace mavira::ui {

/// Multi-tab native settings dialog:
/// 1. General & Library (9 toggle switches + theme + RAM budget)
/// 2. Cover Storage (Mode 1 RAM vs Mode 2 covers.pack builder & purge)
/// 3. Keyboard Shortcuts (interactive key rebind & reset to defaults)
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(
        mavira::data::config::AppSettings currentSettings,
        mavira::data::db::DatabaseManager* db,
        mavira::core::cache::CacheManager* cacheManager,
        const mavira::data::fs::PortablePaths& portablePaths,
        QWidget* parent = nullptr);

    ~SettingsDialog() override;

    [[nodiscard]] mavira::data::config::AppSettings settings() const;

    // Accessors for UI integration & testing
    [[nodiscard]] QTabWidget* tabWidget() const noexcept { return tabs_; }
    [[nodiscard]] QRadioButton* mode1Radio() const noexcept { return rbMode1Ram_; }
    [[nodiscard]] QRadioButton* mode2Radio() const noexcept { return rbMode2Pack_; }

public slots:
    void buildCoversPack();
    void purgeCoverCache();
    void resetShortcutsToDefaults();

signals:
    void settingsApplied(const mavira::data::config::AppSettings& settings);
    void coverCachePurged();
    void coversPackBuilt();

private:
    void setupUi();
    void loadFromSettings(const mavira::data::config::AppSettings& s);

    mavira::data::config::AppSettings workingSettings_{};
    mavira::data::db::DatabaseManager* db_{nullptr};
    mavira::core::cache::CacheManager* cacheManager_{nullptr};
    mavira::data::fs::PortablePaths portablePaths_{};

    QTabWidget* tabs_{nullptr};

    // Tab 1: General & Library
    QCheckBox* chkDefaultMangaMode_{nullptr};
    QCheckBox* chkSmartWideSpread_{nullptr};
    QCheckBox* chkAutoCropBorders_{nullptr};
    QCheckBox* chkVerticalWebtoonMode_{nullptr};
    QCheckBox* chkKeepScreenActive_{nullptr};
    QCheckBox* chkCircularProgressBar_{nullptr};
    QCheckBox* chkSmartRename_{nullptr};
    QCheckBox* chkShowHiddenItems_{nullptr};
    QCheckBox* chkLoupeMagnifier_{nullptr};
    QComboBox* cmbTheme_{nullptr};
    QSpinBox* spnRamLimitMb_{nullptr};
    QLineEdit* editExternalAppPath_{nullptr};
    QPushButton* btnBrowseExternalApp_{nullptr};

    // Tab 2: Cover Storage
    QRadioButton* rbMode1Ram_{nullptr};
    QRadioButton* rbMode2Pack_{nullptr};
    QPushButton* btnBuildPack_{nullptr};
    QPushButton* btnPurgeCache_{nullptr};
    QProgressBar* packProgressBar_{nullptr};
    QLabel* packStatusLabel_{nullptr};

    // Tab 3: Keyboard Shortcuts
    std::map<std::string, QKeySequenceEdit*> shortcutEditors_{};
    QPushButton* btnResetShortcuts_{nullptr};

    std::stop_source packStopSource_{};
    std::jthread packWorkerThread_{};
    std::atomic<bool> buildingPack_{false};
};

} // namespace mavira::ui
