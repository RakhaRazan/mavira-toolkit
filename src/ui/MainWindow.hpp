#pragma once

#include "app/ApplicationCore.hpp"
#include "app/LibraryManager.hpp"
#include "app/ReaderController.hpp"
#include "core/cache/CacheManager.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "ui/ExplorerSidebar.hpp"
#include "ui/LibraryView.hpp"
#include "ui/ReaderViewport.hpp"

#include <QFont>
#include <QMainWindow>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <vector>

class QCloseEvent;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QVBoxLayout;
class QWidget;

namespace mavira::ui {

class PreferencesWorkspace;
class ToolWorkspaceWidget;

enum class ActiveViewMode : int {
    Library = 0,
    Reader = 1,
    ToolWorkspace = 2,
    Preferences = 3
};

/// Main application desktop shell:
/// - High-contrast Dark & Light QSS themes (sharp native geometry, zero bloated rounded cards)
/// - Instant FTS5 search bar
/// - Continue Reading Strip
/// - Random Unread launcher
/// - Native Windows Explorer folder/file picker (QFileDialog, asynchronous scanning via LibraryManager)
/// - View switcher between Library and Reader views
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(
        mavira::data::db::DatabaseManager* db,
        mavira::core::cache::CacheManager* cacheManager,
        mavira::app::LibraryManager* libraryManager,
        mavira::data::config::SettingsManager* settingsManager = nullptr,
        QWidget* parent = nullptr);

    ~MainWindow() override;

    // -------------------------------------------------------------------------
    // Theme & View Switching
    // -------------------------------------------------------------------------
    void applyTheme(const std::string& themeName, bool persist = true);
    void toggleTheme();
    [[nodiscard]] std::string currentTheme() const noexcept { return currentTheme_; }

    void applySettings(const mavira::data::config::AppSettings& newSettings, bool persist = true);
    [[nodiscard]] const mavira::data::config::AppSettings& currentSettings() const noexcept {
        return settings_;
    }

    void switchToView(ActiveViewMode mode);
    [[nodiscard]] ActiveViewMode activeViewMode() const noexcept { return activeViewMode_; }

    // -------------------------------------------------------------------------
    // Library Actions & Refresh
    // -------------------------------------------------------------------------
    void refreshLibraryView();
    void refreshContinueReadingStrip();
    void executeSearch(const QString& queryText);
    void triggerRandomUnread();
    void openComicInReader(const mavira::data::db::ComicRecord& comic);
    void openComicPath(const std::filesystem::path& filePath);

    /// Adds a watched folder path asynchronously via LibraryManager without blocking the UI thread.
    void addFolderPathAsync(const std::filesystem::path& folderPath);

    // -------------------------------------------------------------------------
    // Component Accessors (for UI integration & testing)
    // -------------------------------------------------------------------------
    [[nodiscard]] ExplorerSidebar* sidebar() const noexcept { return sidebar_; }
    [[nodiscard]] LibraryView* libraryView() const noexcept { return libraryView_; }
    [[nodiscard]] mavira::app::ApplicationCore* applicationCore() const noexcept { return appCore_; }
    [[nodiscard]] mavira::app::ReaderController* readerController() const noexcept { return readerController_; }
    [[nodiscard]] ReaderViewport* readerViewport();
    [[nodiscard]] ToolWorkspaceWidget* toolWorkspace();
    [[nodiscard]] PreferencesWorkspace* preferencesWorkspace();
    [[nodiscard]] QLineEdit* searchBar() const noexcept { return searchEdit_; }
    [[nodiscard]] QListWidget* continueReadingList() const noexcept { return continueReadingList_; }
    [[nodiscard]] const std::vector<mavira::data::db::ComicRecord>& displayedComics() const noexcept {
        return displayedComics_;
    }

    [[nodiscard]] static QString buildStylesheet(std::string_view themeName);
    [[nodiscard]] static QString buildStylesheet(std::string_view themeName, int uiScalePercent);

public slots:
    void promptAddWatchedFolder();
    void promptOpenComicFile();
    void openToolWorkspace(
        const QString& toolId,
        const std::vector<std::filesystem::path>& initialPaths = {});
    void openPreferencesWorkspace();
    void openMergeComicsDialog(const std::vector<std::filesystem::path>& initialFiles = {});
    void openConvertComicDialog();
    void openBatchConvertImagesDialog();
    void openCreateComicDialog();
    void openExtractPagesDialog();
    void openNativeOcrDialog();
    void openShortcutsDialog();
    void openSettingsDialog();
    void openAboutDialog();

signals:
    void comicOpenedInReader(qint64 comicId, const QString& filePath);
    void themeChanged(const QString& themeName);
    void viewModeChanged(mavira::ui::ActiveViewMode mode);

private slots:
    void onSidebarNavigationSelected(const mavira::ui::SidebarFilterSelection& selection);
    void onSearchTextChanged(const QString& text);
    void onContinueReadingItemActivated(QListWidgetItem* item);
    void onScanProgress(int processed, int total, const QString& currentFolder);
    void onScanFinished(int indexedCount, int prunedCount, bool cancelled);

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUi();
    void setupMenuBar();
    void retranslateUi();
    void applyUiScale(int percent);
    void updateScreenKeepAlive();
    void refreshTaskbarJumpList();
    void populateLibraryTable(
        const std::vector<mavira::data::db::ComicRecord>& comics,
        const QString& viewTitle);
    void applySortAndStatusFilter();
    [[nodiscard]] std::vector<std::filesystem::path> selectedLibraryPaths() const;
    ReaderViewport* ensureReaderViewport();
    ToolWorkspaceWidget* ensureToolWorkspace();
    PreferencesWorkspace* ensurePreferencesWorkspace();
    std::vector<mavira::data::db::ComicRecord> evaluateSmartViewFilter(const QString& queryJson);

    mavira::data::db::DatabaseManager* db_{nullptr};
    mavira::core::cache::CacheManager* cacheManager_{nullptr};
    mavira::app::LibraryManager* libraryManager_{nullptr};
    mavira::data::config::SettingsManager* settingsManager_{nullptr};
    mavira::app::ApplicationCore* appCore_{nullptr};
    mavira::data::config::AppSettings settings_{};

    std::string currentTheme_{"obsidian_cyber"};
    QFont baseAppFont_{};
    ActiveViewMode activeViewMode_{ActiveViewMode::Library};
    ActiveViewMode previousViewMode_{ActiveViewMode::Library};

    // Menus
    QMenu* fileMenu_{nullptr};
    QMenu* editMenu_{nullptr};
    QMenu* toolsMenu_{nullptr};
    QMenu* viewMenu_{nullptr};
    QMenu* helpMenu_{nullptr};

    // Top command bar
    QPushButton* btnAddFolder_{nullptr};
    QPushButton* btnOpenFile_{nullptr};
    QPushButton* btnMergeComics_{nullptr};
    QPushButton* btnRandomUnread_{nullptr};
    QLineEdit* searchEdit_{nullptr};
    QComboBox* cmbSortBy_{nullptr};
    QComboBox* cmbStatusFilter_{nullptr};
    QPushButton* btnLibraryView_{nullptr};
    QPushButton* btnReaderView_{nullptr};
    QPushButton* btnSettings_{nullptr};
    QPushButton* btnAbout_{nullptr};

    // Main layout
    QSplitter* mainSplitter_{nullptr};
    ExplorerSidebar* sidebar_{nullptr};
    QStackedWidget* centralStack_{nullptr};

    // Library Page
    QWidget* libraryPage_{nullptr};
    QWidget* continueReadingContainer_{nullptr};
    QLabel* continueReadingTitleLabel_{nullptr};
    QListWidget* continueReadingList_{nullptr};
    QLabel* viewHeaderLabel_{nullptr};
    LibraryView* libraryView_{nullptr};

    // Reader Page shell
    QWidget* readerPage_{nullptr};
    QVBoxLayout* readerLayout_{nullptr};
    QLabel* readerTitleLabel_{nullptr};
    QPushButton* btnBackToLibrary_{nullptr};
    mavira::app::ReaderController* readerController_{nullptr};
    ReaderViewport* readerViewport_{nullptr};

    // Full-area Tool & Preferences Workspaces (lazy-instantiated on first use)
    QWidget* toolWorkspaceHost_{nullptr};
    QWidget* prefsWorkspaceHost_{nullptr};
    ToolWorkspaceWidget* toolWorkspace_{nullptr};
    PreferencesWorkspace* prefsWorkspace_{nullptr};

    // Status bar widgets
    QLabel* statusLabel_{nullptr};
    QProgressBar* scanProgressBar_{nullptr};

    std::vector<mavira::data::db::ComicRecord> unfilteredComics_{};
    std::vector<mavira::data::db::ComicRecord> displayedComics_{};
    std::vector<mavira::data::db::ComicRecord> continueReadingComics_{};
    QString currentViewTitle_{};
    bool isAllComicsView_{true};
};

} // namespace mavira::ui
