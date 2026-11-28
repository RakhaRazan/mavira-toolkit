#include "ui/LibraryView.hpp"

#include "app/ApplicationCore.hpp"
#include "core/archive/ComicMerger.hpp"
#include "core/metadata/MetadataManager.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/MergeComicsDialog.hpp"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QFileDialog>
#include <QImage>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

namespace mavira::ui {

namespace {

constexpr int kPrefetchRowBuffer = 12;
constexpr std::size_t kMaxUiPixmapEntries = 400;

[[nodiscard]] QString formatLabelForComic(mavira::core::ComicFormat fmt) {
    const auto sv = mavira::core::toString(fmt);
    return QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
}

[[nodiscard]] std::int64_t currentUnixSeconds() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

} // namespace

// =============================================================================
// LibraryModel
// =============================================================================
LibraryModel::LibraryModel(QObject* parent)
    : QAbstractListModel(parent) {}

void LibraryModel::setComics(std::vector<mavira::data::db::ComicRecord> comics) {
    beginResetModel();
    comics_ = std::move(comics);
    idToRow_.clear();
    idToRow_.reserve(comics_.size());
    for (std::size_t i = 0; i < comics_.size(); ++i) {
        idToRow_[comics_[i].id] = static_cast<int>(i);
    }
    endResetModel();
}

void LibraryModel::setSmartRename(bool enabled) {
    if (smartRename_ == enabled) {
        return;
    }
    smartRename_ = enabled;
    if (!comics_.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(comics_.size()) - 1), {Qt::DisplayRole});
    }
}

void LibraryModel::setCoverPixmap(std::int64_t comicId, const QPixmap& pixmap) {
    pixmaps_.insert(comicId, pixmap);
    const int row = rowForComicId(comicId);
    if (row >= 0) {
        const QModelIndex idx = index(row);
        emit dataChanged(idx, idx, {Qt::DecorationRole});
    }
}

void LibraryModel::evictPixmapsOutsideRange(int minKeepRow, int maxKeepRow) {
    if (pixmaps_.size() <= static_cast<qsizetype>(kMaxUiPixmapEntries)) {
        return;
    }
    for (auto it = pixmaps_.begin(); it != pixmaps_.end();) {
        const int row = rowForComicId(static_cast<std::int64_t>(it.key()));
        if (row < minKeepRow || row > maxKeepRow) {
            it = pixmaps_.erase(it);
        } else {
            ++it;
        }
    }
}

void LibraryModel::clearCoverPixmaps() {
    pixmaps_.clear();
    if (!comics_.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(comics_.size()) - 1), {Qt::DecorationRole});
    }
}

bool LibraryModel::hasCoverPixmap(std::int64_t comicId) const {
    return pixmaps_.contains(comicId);
}

const mavira::data::db::ComicRecord* LibraryModel::comicAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= comics_.size()) {
        return nullptr;
    }
    return &comics_[static_cast<std::size_t>(row)];
}

int LibraryModel::rowForComicId(std::int64_t comicId) const {
    auto it = idToRow_.find(comicId);
    return it == idToRow_.end() ? -1 : it->second;
}

int LibraryModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(comics_.size());
}

QVariant LibraryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= comics_.size()) {
        return {};
    }

    const auto& comic = comics_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole: {
            std::string raw = !comic.title.empty()
                ? comic.title
                : mavira::data::fs::FileManager::wideToUtf8(comic.filePath.stem().native());
            if (smartRename_) {
                const std::string cleaned =
                    mavira::core::metadata::MetadataManager::cleanDisplayTitle(raw);
                if (!cleaned.empty()) {
                    raw = cleaned;
                }
            }
            return QString::fromStdString(raw);
        }
        case Qt::ToolTipRole:
            return QString::fromStdWString(comic.filePath.native());
        case Qt::DecorationRole: {
            auto it = pixmaps_.constFind(comic.id);
            if (it != pixmaps_.constEnd()) {
                return *it;
            }
            return {};
        }
        case RoleComicId:
            return static_cast<qint64>(comic.id);
        case RoleFormatText:
            return formatLabelForComic(comic.format);
        case RolePageCount:
            return static_cast<quint32>(comic.pageCount);
        case RoleCurrentPage:
            return static_cast<quint32>(comic.currentPage);
        case RoleIsRead:
            return comic.isRead;
        case RoleIsOfflinePinned:
            return comic.isOfflinePinned;
        case RoleIsHidden:
            return comic.isHidden;
        default:
            return {};
    }
}

// =============================================================================
// LibraryView
// =============================================================================
LibraryView::LibraryView(
    mavira::data::db::DatabaseManager* db,
    mavira::core::cache::CacheManager* cacheManager,
    mavira::app::LibraryManager* libraryManager,
    QWidget* parent)
    : QWidget(parent),
      db_(db),
      cacheManager_(cacheManager),
      libraryManager_(libraryManager) {
    setObjectName(QStringLiteral("LibraryView"));
    setupUi();
}

LibraryView::~LibraryView() {
    shuttingDown_.store(true, std::memory_order_release);
    if (scrollDebounceTimer_ != nullptr) {
        scrollDebounceTimer_->stop();
    }
    cancelAllCoverTasks();
    coverPool_.shutdown();
}

void LibraryView::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    model_ = new LibraryModel(this);
    delegate_ = new ComicItemDelegate(this);

    listView_ = new QListView(this);
    listView_->setObjectName(QStringLiteral("VirtualizedLibraryGrid"));
    listView_->setModel(model_);
    listView_->setItemDelegate(delegate_);

    // Virtualized 30,000+ item IconMode configuration
    listView_->setViewMode(QListView::IconMode);
    listView_->setUniformItemSizes(true);
    listView_->setLayoutMode(QListView::Batched);
    listView_->setBatchSize(100);
    listView_->setResizeMode(QListView::Adjust);
    listView_->setMovement(QListView::Static);
    listView_->setSpacing(4);
    listView_->setGridSize(QSize(delegate_->cardWidth(), delegate_->cardHeight()));
    listView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    listView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    listView_->setContextMenuPolicy(Qt::CustomContextMenu);

    layout->addWidget(listView_, 1);

    scrollDebounceTimer_ = new QTimer(this);
    scrollDebounceTimer_->setSingleShot(true);
    scrollDebounceTimer_->setInterval(kScrollDebounceMs);

    connect(scrollDebounceTimer_, &QTimer::timeout, this, &LibraryView::onScrollDebounced);
    connect(listView_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!shuttingDown_.load(std::memory_order_acquire)) {
            scrollDebounceTimer_->start(kScrollDebounceMs);
        }
    });
    connect(listView_, &QListView::activated, this, &LibraryView::onItemActivated);
    connect(listView_, &QListView::doubleClicked, this, &LibraryView::onItemActivated);
    connect(
        listView_,
        &QListView::customContextMenuRequested,
        this,
        &LibraryView::onContextMenuRequested);
}

void LibraryView::setComics(std::vector<mavira::data::db::ComicRecord> comics) {
    cancelAllCoverTasks();
    model_->setComics(std::move(comics));
    scrollDebounceTimer_->start(kScrollDebounceMs);
}

void LibraryView::setDisplayOptions(bool showCircularProgress, bool smartRename) {
    delegate_->setShowCircularProgress(showCircularProgress);
    model_->setSmartRename(smartRename);
    listView_->viewport()->update();
}

void LibraryView::setCardDensity(std::string_view density) {
    delegate_->setCardDensity(density);
    listView_->setGridSize(QSize(delegate_->cardWidth(), delegate_->cardHeight()));
    listView_->doItemsLayout();
    listView_->viewport()->update();
}

void LibraryView::setDarkTheme(bool dark) {
    delegate_->setDarkTheme(dark);
    listView_->viewport()->update();
}

void LibraryView::setThemePalette(
    bool dark,
    const QColor& cardBg,
    const QColor& coverBg,
    const QColor& accentCol,
    const QColor& borderCol,
    const QColor& hoverBorderCol,
    const QColor& textCol,
    const QColor& mutedTextCol) {
    delegate_->setThemePalette(
        dark, cardBg, coverBg, accentCol, borderCol, hoverBorderCol, textCol, mutedTextCol);
    listView_->viewport()->update();
}

void LibraryView::setExternalAppPath(const std::filesystem::path& appPath) {
    externalAppPath_ = appPath;
}

void LibraryView::clearCoverCache() {
    cancelAllCoverTasks();
    model_->clearCoverPixmaps();
    if (cacheManager_ != nullptr && cacheManager_->coverStorage() != nullptr) {
        cacheManager_->coverStorage()->clearMemoryCache();
    }
    scrollDebounceTimer_->start(kScrollDebounceMs);
}

void LibraryView::selectAll() {
    if (listView_ != nullptr) {
        listView_->selectAll();
    }
}

void LibraryView::removeSelectedFromLibrary(bool deleteFromDisk) {
    const auto selected = selectedComics();
    if (selected.empty() || libraryManager_ == nullptr) {
        return;
    }

    if (deleteFromDisk) {
        const QString prompt = (selected.size() == 1)
            ? QStringLiteral("Move \"%1\" to the Windows Recycle Bin and remove it from the library?")
                  .arg(QString::fromStdString(selected.front().title))
            : QStringLiteral("Move %1 selected files to the Windows Recycle Bin and remove them from the library?")
                  .arg(selected.size());
        if (QMessageBox::question(
                this,
                QStringLiteral("Delete to Recycle Bin"),
                prompt,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
    }

    std::size_t removedCount = 0;
    for (const auto& c : selected) {
        auto res = libraryManager_->removeComicFromLibrary(c.id, deleteFromDisk);
        if (res.isOk()) {
            ++removedCount;
        } else {
            emit statusMessage(
                QStringLiteral("Failed to remove \"%1\": %2")
                    .arg(QString::fromStdString(c.title))
                    .arg(QString::fromStdString(res.error().message)));
        }
    }

    if (removedCount > 0) {
        emit statusMessage(
            deleteFromDisk
                ? QStringLiteral("Moved %1 comic(s) to Recycle Bin.").arg(removedCount)
                : QStringLiteral("Removed %1 comic(s) from library.").arg(removedCount));
    }
}

std::vector<mavira::data::db::ComicRecord> LibraryView::selectedComics() const {
    std::vector<mavira::data::db::ComicRecord> out;
    if (listView_->selectionModel() == nullptr) {
        return out;
    }
    const QModelIndexList indices = listView_->selectionModel()->selectedIndexes();
    out.reserve(static_cast<std::size_t>(indices.size()));
    for (const QModelIndex& idx : indices) {
        if (const auto* c = model_->comicAt(idx.row())) {
            out.push_back(*c);
        }
    }
    return out;
}

std::pair<int, int> LibraryView::computeVisibleRowRange() const {
    const int total = model_->rowCount();
    if (total <= 0) {
        return {0, -1};
    }

    const QRect vp = listView_->viewport()->rect();
    if (vp.isEmpty()) {
        return {0, std::min(total - 1, 24)};
    }

    const int vpWidth = std::max(1, vp.width());
    const int cardW = (delegate_ != nullptr) ? delegate_->cardWidth() : ComicItemDelegate::kCardWidth;
    const int cardH = (delegate_ != nullptr) ? delegate_->cardHeight() : ComicItemDelegate::kCardHeight;
    const int cols = std::max(1, vpWidth / (cardW + listView_->spacing()));
    const int rowHeight = cardH + listView_->spacing();
    const int scrollY = listView_->verticalScrollBar()->value();

    const int firstVisualRow = std::max(0, scrollY / std::max(1, rowHeight));
    const int lastVisualRow = (scrollY + vp.height()) / std::max(1, rowHeight) + 1;

    const int firstIdx = std::clamp(firstVisualRow * cols, 0, total - 1);
    const int lastIdx = std::clamp((lastVisualRow + 1) * cols - 1, firstIdx, total - 1);
    return {firstIdx, lastIdx};
}

void LibraryView::cancelOffscreenCoverTasks(int minKeepRow, int maxKeepRow) {
    for (auto it = activeCoverTasks_.begin(); it != activeCoverTasks_.end();) {
        if (it->second.row < minKeepRow || it->second.row > maxKeepRow) {
            it->second.handle.cancel();
            it = activeCoverTasks_.erase(it);
        } else {
            ++it;
        }
    }
}

void LibraryView::cancelAllCoverTasks() {
    for (auto& [id, task] : activeCoverTasks_) {
        task.handle.cancel();
    }
    activeCoverTasks_.clear();
}

void LibraryView::onScrollDebounced() {
    requestVisibleCovers();
}

void LibraryView::requestVisibleCovers() {
    if (shuttingDown_.load(std::memory_order_acquire) ||
        cacheManager_ == nullptr || cacheManager_->coverStorage() == nullptr) {
        return;
    }

    const auto [firstRow, lastRow] = computeVisibleRowRange();
    if (lastRow < firstRow) {
        return;
    }

    const int total = model_->rowCount();
    const int keepMin = std::max(0, firstRow - kPrefetchRowBuffer);
    const int keepMax = std::min(total - 1, lastRow + kPrefetchRowBuffer);

    // Cancel any Mode 1 (or in-flight) cover extraction tasks that scrolled off-screen.
    cancelOffscreenCoverTasks(keepMin, keepMax);
    model_->evictPixmapsOutsideRange(keepMin, keepMax);

    auto* storage = cacheManager_->coverStorage();
    for (int row = firstRow; row <= lastRow; ++row) {
        const auto* comic = model_->comicAt(row);
        if (comic == nullptr || comic->id <= 0) {
            continue;
        }
        if (model_->hasCoverPixmap(comic->id) ||
            activeCoverTasks_.find(comic->id) != activeCoverTasks_.end()) {
            continue;
        }

        const auto comicCopy = *comic;
        const std::int64_t comicId = comicCopy.id;

        auto handle = coverPool_.enqueue(
            mavira::core::RequestPriority::Normal,
            [this, storage, comicCopy, comicId](std::stop_token stopToken) {
                if (stopToken.stop_requested() || shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                auto res = storage->getCover(comicCopy, stopToken);
                QImage decodedImg;
                if (res && res.value() != nullptr && res.value()->isValid() &&
                    !stopToken.stop_requested()) {
                    const auto& frame = *res.value();
                    QImage wrapper(
                        frame.pixels.data(),
                        static_cast<int>(frame.width),
                        static_cast<int>(frame.height),
                        static_cast<int>(frame.effectiveStride()),
                        QImage::Format_RGBA8888);
                    decodedImg = wrapper.copy();
                }

                QMetaObject::invokeMethod(
                    this,
                    [this, comicId, decodedImg = std::move(decodedImg)]() {
                        activeCoverTasks_.erase(comicId);
                        if (!shuttingDown_.load(std::memory_order_acquire) && !decodedImg.isNull()) {
                            model_->setCoverPixmap(comicId, QPixmap::fromImage(decodedImg));
                        }
                    },
                    Qt::QueuedConnection);
            });

        activeCoverTasks_.emplace(comicId, ActiveCoverTask{row, std::move(handle)});
    }
}

void LibraryView::onItemActivated(const QModelIndex& index) {
    if (const auto* comic = model_->comicAt(index.row())) {
        emit comicActivated(*comic);
    }
}

void LibraryView::onContextMenuRequested(const QPoint& pos) {
    const QModelIndex clickedIdx = listView_->indexAt(pos);
    if (!clickedIdx.isValid()) {
        return;
    }

    if (!listView_->selectionModel()->isSelected(clickedIdx)) {
        listView_->setCurrentIndex(clickedIdx);
    }

    const auto selected = selectedComics();
    if (selected.empty()) {
        return;
    }
    const auto primary = selected.front();

    QMenu menu(this);
    menu.addAction(QStringLiteral("Open in Reader"), this, [this, primary]() {
        emit comicActivated(primary);
    });

    menu.addSeparator();

    // Add to Shelf submenu
    if (db_ != nullptr && db_->isOpen()) {
        QMenu* shelfMenu = menu.addMenu(QStringLiteral("Add to Shelf"));
        if (auto shelvesRes = db_->getShelves(); shelvesRes) {
            for (const auto& shelf : shelvesRes.value()) {
                if (shelf.name == "Favorites") {
                    continue;
                }
                shelfMenu->addAction(
                    QString::fromStdString(shelf.name),
                    this,
                    [this, shelfId = shelf.id, selected]() {
                        const std::int64_t now = currentUnixSeconds();
                        for (const auto& c : selected) {
                            (void)db_->addComicToShelf(shelfId, c.id, 0, now);
                        }
                        emit libraryMutated();
                    });
            }
        }
        if (shelfMenu->isEmpty()) {
            QAction* noneAct = shelfMenu->addAction(QStringLiteral("(No custom shelves yet)"));
            noneAct->setEnabled(false);
        }

        // Toggle Favorite
        menu.addAction(QStringLiteral("Toggle Favorite"), this, [this, selected]() {
            auto shelfRes = db_->createShelf("Favorites", -100, currentUnixSeconds());
            if (!shelfRes) {
                return;
            }
            const std::int64_t favId = shelfRes.value();
            auto existingRes = db_->getComicsInShelf(favId, true);
            std::unordered_map<std::int64_t, bool> inFav;
            if (existingRes) {
                for (const auto& fc : existingRes.value()) {
                    inFav[fc.id] = true;
                }
            }
            const std::int64_t now = currentUnixSeconds();
            for (const auto& c : selected) {
                if (inFav.contains(c.id)) {
                    (void)db_->removeComicFromShelf(favId, c.id);
                } else {
                    (void)db_->addComicToShelf(favId, c.id, 0, now);
                }
            }
            emit libraryMutated();
        });

        // Toggle Hide
        const QString hideLabel = primary.isHidden
            ? QStringLiteral("Unhide Volume")
            : QStringLiteral("Hide Volume");
        menu.addAction(hideLabel, this, [this, selected]() {
            for (const auto& c : selected) {
                (void)db_->setComicHidden(c.id, !c.isHidden);
            }
            emit libraryMutated();
        });
    }

    // Pin / Unpin Offline
    if (libraryManager_ != nullptr) {
        const QString pinLabel = primary.isOfflinePinned
            ? QStringLiteral("Unpin Offline")
            : QStringLiteral("Pin Offline");
        menu.addAction(pinLabel, this, [this, selected]() {
            for (const auto& c : selected) {
                if (c.isOfflinePinned) {
                    libraryManager_->unpinComicOffline(c.id);
                } else {
                    libraryManager_->pinComicOffline(c.id);
                }
            }
        });
    }

    menu.addSeparator();

    // Extract Pages
    menu.addAction(QStringLiteral("Extract Pages..."), this, [this, primary]() {
        const std::filesystem::path srcPath = (libraryManager_ != nullptr)
            ? libraryManager_->resolveReadablePath(primary)
            : primary.filePath;
        emit toolWorkspaceRequested(QStringLiteral("extract_pages"), {srcPath});
    });

    // Merge Selected
    QAction* mergeAct = menu.addAction(QStringLiteral("Merge Selected Comics..."), this, [this, selected]() {
        std::vector<std::filesystem::path> inputs;
        inputs.reserve(selected.size());
        for (const auto& c : selected) {
            inputs.push_back(
                libraryManager_ ? libraryManager_->resolveReadablePath(c) : c.filePath);
        }
        emit toolWorkspaceRequested(QStringLiteral("merge_comics"), inputs);
    });
    mergeAct->setEnabled(!selected.empty());

    // Open in External App
    menu.addAction(QStringLiteral("Open in External App"), this, [this, primary]() {
        const std::filesystem::path target = (libraryManager_ != nullptr)
            ? libraryManager_->resolveReadablePath(primary)
            : primary.filePath;
        if (!externalAppPath_.empty()) {
            auto launchRes = mavira::app::ApplicationCore::launchExternalApp(target, externalAppPath_);
            if (launchRes) {
                emit statusMessage(
                    QStringLiteral("Launched external app (PID %1).").arg(launchRes.value()));
            } else {
                emit statusMessage(
                    QStringLiteral("External app handoff failed: %1")
                        .arg(QString::fromStdString(launchRes.error().message)));
            }
            return;
        }
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdWString(target.native())));
    });

    if (libraryManager_ != nullptr) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove from Library"), this, [this]() {
            removeSelectedFromLibrary(false);
        });
        menu.addAction(QStringLiteral("Delete File (Recycle Bin)"), this, [this]() {
            removeSelectedFromLibrary(true);
        });
    }

    menu.exec(listView_->viewport()->mapToGlobal(pos));
}

} // namespace mavira::ui
