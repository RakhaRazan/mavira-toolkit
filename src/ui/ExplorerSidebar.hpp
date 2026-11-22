#pragma once

#include "app/LibraryManager.hpp"
#include "data/db/DatabaseManager.hpp"

#include <QString>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <vector>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace mavira::ui {

enum class SidebarNodeKind : int {
    SectionHeader = 0,
    AllComics,
    Favorites,
    WatchedFolder,
    ChildFolder,
    SeriesBucket,
    SeriesItem,
    ShelfItem,
    SmartViewPreset,
    SmartViewCustom,
    CloudSource,
    ToolItem
};

struct SidebarFilterSelection {
    SidebarNodeKind kind{SidebarNodeKind::AllComics};
    std::int64_t recordId{0};
    QString label{};
    QString payload{}; // Folder path, series name, or SmartView JSON query
};

/// Native high-contrast navigation sidebar providing trees/lists for:
/// - All Comics
/// - Folder / Series Browser (A-Z)
/// - Favorites
/// - Shelves (Create / Rename / Delete custom shelves)
/// - Smartviews (Built-in presets + Custom Rule Builder modal)
/// - Detected Windows Cloud Sources
class ExplorerSidebar : public QWidget {
    Q_OBJECT

public:
    ExplorerSidebar(
        mavira::data::db::DatabaseManager* db,
        mavira::app::LibraryManager* libraryManager,
        QWidget* parent = nullptr);

    ~ExplorerSidebar() override = default;

    /// Rebuilds the navigation tree from SQLite & cloud sync state without scanning disks.
    void refreshNavigation();
    void retranslateUi();

    ///Programmatically selects a top-level navigation node (e.g. AllComics or Favorites).
    void selectAllComics();
    void selectFavorites();

    /// Triggers interactive dialogs/actions for shelves & smartviews (also callable from tests/menus).
    std::int64_t createCustomShelfByName(const QString& shelfName);
    bool renameCustomShelfById(std::int64_t shelfId, const QString& newName);
    bool deleteCustomShelfById(std::int64_t shelfId);
    std::int64_t createCustomSmartView(const QString& name, const QString& queryJson);
    bool deleteCustomSmartViewById(std::int64_t smartViewId);

    [[nodiscard]] SidebarFilterSelection currentSelection() const noexcept {
        return currentSelection_;
    }

    [[nodiscard]] QTreeWidget* treeWidget() const noexcept {
        return tree_;
    }

public slots:
    void promptCreateShelf();
    void promptRenameSelectedShelf();
    void promptDeleteSelectedItem();
    void openSmartViewBuilderModal();

signals:
    void navigationSelected(const mavira::ui::SidebarFilterSelection& selection);
    void cloudFolderWatchRequested(const QString& folderPath);
    void toolRequested(const QString& toolId);

private slots:
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onItemExpanded(QTreeWidgetItem* item);
    void onCustomContextMenu(const QPoint& pos);

private:
    void setupUi();
    void populateFolderChildren(QTreeWidgetItem* folderItem, const std::filesystem::path& folderPath);
    std::int64_t ensureFavoritesShelfId();

    mavira::data::db::DatabaseManager* db_{nullptr};
    mavira::app::LibraryManager* libraryManager_{nullptr};

    QTreeWidget* tree_{nullptr};
    QPushButton* btnNewShelf_{nullptr};
    QPushButton* btnRenameShelf_{nullptr};
    QPushButton* btnNewSmartView_{nullptr};
    QPushButton* btnDeleteItem_{nullptr};

    QTreeWidgetItem* allComicsItem_{nullptr};
    QTreeWidgetItem* favoritesItem_{nullptr};

    std::int64_t favoritesShelfId_{0};
    SidebarFilterSelection currentSelection_{};
};

} // namespace mavira::ui
