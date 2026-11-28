#pragma once

#include "app/LibraryManager.hpp"
#include "core/cache/CacheManager.hpp"
#include "core/threading/WorkerPool.hpp"
#include "data/db/DatabaseManager.hpp"
#include "ui/ComicItemDelegate.hpp"

#include <QAbstractListModel>
#include <QHash>
#include <QListView>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string_view>
#include <unordered_map>
#include <vector>

class QTimer;

namespace mavira::ui {

/// Virtualized list model backed by `std::vector<ComicRecord>` for 30,000+ volumes.
class LibraryModel : public QAbstractListModel {
    Q_OBJECT

public:
    explicit LibraryModel(QObject* parent = nullptr);

    void setComics(std::vector<mavira::data::db::ComicRecord> comics);
    void setSmartRename(bool enabled);
    [[nodiscard]] bool smartRename() const noexcept { return smartRename_; }

    void setCoverPixmap(std::int64_t comicId, const QPixmap& pixmap);
    void evictPixmapsOutsideRange(int minKeepRow, int maxKeepRow);
    void clearCoverPixmaps();
    [[nodiscard]] bool hasCoverPixmap(std::int64_t comicId) const;

    [[nodiscard]] const std::vector<mavira::data::db::ComicRecord>& comics() const noexcept {
        return comics_;
    }
    [[nodiscard]] const mavira::data::db::ComicRecord* comicAt(int row) const noexcept;
    [[nodiscard]] int rowForComicId(std::int64_t comicId) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

private:
    std::vector<mavira::data::db::ComicRecord> comics_{};
    std::unordered_map<std::int64_t, int> idToRow_{};
    QHash<qint64, QPixmap> pixmaps_{};
    bool smartRename_{false};
};

/// Virtualized 30,000+ item comic grid using `QListView` in IconMode
/// (`setUniformItemSizes(true)`, `setLayoutMode(QListView::Batched)`).
/// Debounces viewport scroll events (75 ms) to cancel off-screen Mode 1 cover tasks
/// and provides full right-click context actions.
class LibraryView : public QWidget {
    Q_OBJECT

public:
    static constexpr int kScrollDebounceMs = 75;

    LibraryView(
        mavira::data::db::DatabaseManager* db,
        mavira::core::cache::CacheManager* cacheManager,
        mavira::app::LibraryManager* libraryManager,
        QWidget* parent = nullptr);

    ~LibraryView() override;

    void setComics(std::vector<mavira::data::db::ComicRecord> comics);
    void setDisplayOptions(bool showCircularProgress, bool smartRename);
    void setCardDensity(std::string_view density);
    void setDarkTheme(bool dark);
    void setThemePalette(
        bool dark,
        const QColor& cardBg,
        const QColor& coverBg,
        const QColor& accentCol,
        const QColor& borderCol,
        const QColor& hoverBorderCol,
        const QColor& textCol,
        const QColor& mutedTextCol);
    void setExternalAppPath(const std::filesystem::path& appPath);
    void clearCoverCache();
    void selectAll();
    void removeSelectedFromLibrary(bool deleteFromDisk);

    /// Triggers cover loading for the currently visible viewport items and cancels
    /// off-screen Mode 1 cover tasks.
    void requestVisibleCovers();

    [[nodiscard]] const std::vector<mavira::data::db::ComicRecord>& comics() const noexcept {
        return model_->comics();
    }
    [[nodiscard]] std::vector<mavira::data::db::ComicRecord> selectedComics() const;
    [[nodiscard]] QListView* listView() const noexcept { return listView_; }
    [[nodiscard]] LibraryModel* model() const noexcept { return model_; }
    [[nodiscard]] ComicItemDelegate* delegate() const noexcept { return delegate_; }
    [[nodiscard]] std::size_t activeCoverTaskCount() const noexcept { return activeCoverTasks_.size(); }

signals:
    void comicActivated(const mavira::data::db::ComicRecord& comic);
    void libraryMutated();
    void statusMessage(const QString& message);
    void toolWorkspaceRequested(
        const QString& toolId,
        const std::vector<std::filesystem::path>& initialPaths);

private slots:
    void onScrollDebounced();
    void onItemActivated(const QModelIndex& index);
    void onContextMenuRequested(const QPoint& pos);

private:
    void setupUi();
    std::pair<int, int> computeVisibleRowRange() const;
    void cancelOffscreenCoverTasks(int minKeepRow, int maxKeepRow);
    void cancelAllCoverTasks();

    mavira::data::db::DatabaseManager* db_{nullptr};
    mavira::core::cache::CacheManager* cacheManager_{nullptr};
    mavira::app::LibraryManager* libraryManager_{nullptr};
    std::filesystem::path externalAppPath_{};

    QListView* listView_{nullptr};
    LibraryModel* model_{nullptr};
    ComicItemDelegate* delegate_{nullptr};
    QTimer* scrollDebounceTimer_{nullptr};

    mavira::core::threading::WorkerPool coverPool_{};
    std::atomic<bool> shuttingDown_{false};

    struct ActiveCoverTask {
        int row{0};
        mavira::core::threading::TaskHandle handle{};
    };
    std::unordered_map<std::int64_t, ActiveCoverTask> activeCoverTasks_{};
};

} // namespace mavira::ui
