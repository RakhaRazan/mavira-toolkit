#include "ui/ExplorerSidebar.hpp"

#include "data/fs/FileManager.hpp"
#include "ui/LocalizationManager.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <cctype>
#include <chrono>
#include <map>

namespace mavira::ui {

namespace {

constexpr int kRoleNodeKind = Qt::UserRole + 1;
constexpr int kRoleRecordId = Qt::UserRole + 2;
constexpr int kRolePayload = Qt::UserRole + 3;
constexpr int kRolePopulated = Qt::UserRole + 4;

constexpr const char* kFavoritesShelfName = "Favorites";

[[nodiscard]] std::int64_t nowUnix() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

[[nodiscard]] QChar seriesBucketLetter(const std::string& series) {
    for (unsigned char ch : series) {
        if (std::isspace(ch)) {
            continue;
        }
        if (std::isalpha(ch)) {
            return QChar(static_cast<char>(std::toupper(ch)));
        }
        return QChar(L'#');
    }
    return QChar(L'#');
}

void configureNodeItem(
    QTreeWidgetItem* item,
    SidebarNodeKind kind,
    std::int64_t recordId = 0,
    const QString& payload = {}) {
    item->setData(0, kRoleNodeKind, static_cast<int>(kind));
    item->setData(0, kRoleRecordId, static_cast<qint64>(recordId));
    item->setData(0, kRolePayload, payload);
}

[[nodiscard]] SidebarNodeKind itemNodeKind(const QTreeWidgetItem* item) {
    if (item == nullptr) {
        return SidebarNodeKind::SectionHeader;
    }
    return static_cast<SidebarNodeKind>(item->data(0, kRoleNodeKind).toInt());
}

} // namespace

ExplorerSidebar::ExplorerSidebar(
    mavira::data::db::DatabaseManager* db,
    mavira::app::LibraryManager* libraryManager,
    QWidget* parent)
    : QWidget(parent),
      db_(db),
      libraryManager_(libraryManager) {
    setObjectName(QStringLiteral("ExplorerSidebar"));
    setupUi();
    refreshNavigation();
}

void ExplorerSidebar::setupUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("SidebarTree"));
    tree_->setHeaderHidden(true);
    tree_->setIndentation(14);
    tree_->setAnimated(false);
    tree_->setUniformRowHeights(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);

    rootLayout->addWidget(tree_, 1);

    connect(tree_, &QTreeWidget::itemClicked, this, &ExplorerSidebar::onItemClicked);
    connect(tree_, &QTreeWidget::itemExpanded, this, &ExplorerSidebar::onItemExpanded);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &ExplorerSidebar::onCustomContextMenu);
}

std::int64_t ExplorerSidebar::ensureFavoritesShelfId() {
    if (db_ == nullptr || !db_->isOpen()) {
        return 0;
    }
    if (favoritesShelfId_ > 0) {
        return favoritesShelfId_;
    }
    if (auto shelvesRes = db_->getShelves(); shelvesRes) {
        for (const auto& shelf : shelvesRes.value()) {
            if (shelf.name == kFavoritesShelfName) {
                favoritesShelfId_ = shelf.id;
                return favoritesShelfId_;
            }
        }
    }
    auto res = db_->createShelf(kFavoritesShelfName, -100, nowUnix());
    if (res) {
        favoritesShelfId_ = res.value();
    }
    return favoritesShelfId_;
}

void ExplorerSidebar::refreshNavigation() {
    tree_->clear();
    allComicsItem_ = nullptr;
    favoritesItem_ = nullptr;

    std::vector<mavira::data::db::ShelfRecord> allShelves;
    if (db_ != nullptr && db_->isOpen()) {
        if (auto shelvesRes = db_->getShelves(); shelvesRes) {
            allShelves = std::move(shelvesRes.value());
            for (const auto& s : allShelves) {
                if (s.name == kFavoritesShelfName) {
                    favoritesShelfId_ = s.id;
                    break;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // 1. LIBRARY ROOT (All Comics & Favorites)
    // -------------------------------------------------------------------------
    auto* libraryHeader = new QTreeWidgetItem(tree_, QStringList{LocalizationManager::tr("side.library")});
    configureNodeItem(libraryHeader, SidebarNodeKind::SectionHeader);
    libraryHeader->setFlags(Qt::ItemIsEnabled);

    std::int64_t totalComics = 0;
    if (db_ != nullptr && db_->isOpen()) {
        if (auto countRes = db_->getTotalComicCount(); countRes) {
            totalComics = countRes.value();
        }
    }

    allComicsItem_ = new QTreeWidgetItem(
        libraryHeader,
        QStringList{QStringLiteral("%1 (%2)").arg(LocalizationManager::tr("side.all_comics")).arg(totalComics)});
    configureNodeItem(allComicsItem_, SidebarNodeKind::AllComics, 0, QStringLiteral("ALL"));

    favoritesItem_ = new QTreeWidgetItem(
        libraryHeader,
        QStringList{LocalizationManager::tr("side.favorites")});
    configureNodeItem(favoritesItem_, SidebarNodeKind::Favorites, favoritesShelfId_, QStringLiteral("FAVORITES"));
    libraryHeader->setExpanded(true);

    // -------------------------------------------------------------------------
    // 2. TOOLS (1-Click Utility Tool Launchers)
    // -------------------------------------------------------------------------
    auto* toolsHeader = new QTreeWidgetItem(tree_, QStringList{LocalizationManager::tr("side.tools")});
    configureNodeItem(toolsHeader, SidebarNodeKind::SectionHeader);
    toolsHeader->setFlags(Qt::ItemIsEnabled);

    struct ToolDef {
        const char* trKey;
        const char* toolId;
    };
    constexpr ToolDef kTools[] = {
        {"tool.convert_comics", "convert_comics"},
        {"tool.convert_images", "convert_images"},
        {"tool.create_comic", "create_comic"},
        {"tool.extract_pages", "extract_pages"},
        {"tool.extract_ocr", "extract_ocr"},
        {"tool.merge_comics", "merge_comics"}
    };
    for (const auto& t : kTools) {
        auto* tItem = new QTreeWidgetItem(toolsHeader, QStringList{LocalizationManager::tr(t.trKey)});
        configureNodeItem(tItem, SidebarNodeKind::ToolItem, 0, QString::fromUtf8(t.toolId));
    }
    toolsHeader->setExpanded(true);

    // -------------------------------------------------------------------------
    // 3. FOLDER / SERIES BROWSER (A-Z)
    // -------------------------------------------------------------------------
    auto* browserHeader = new QTreeWidgetItem(
        tree_, QStringList{LocalizationManager::tr("side.folders_series")});
    configureNodeItem(browserHeader, SidebarNodeKind::SectionHeader);
    browserHeader->setFlags(Qt::ItemIsEnabled);

    auto* foldersRoot = new QTreeWidgetItem(browserHeader, QStringList{LocalizationManager::tr("side.watched_folders")});
    configureNodeItem(foldersRoot, SidebarNodeKind::SectionHeader);

    if (db_ != nullptr && db_->isOpen()) {
        if (auto foldersRes = db_->getWatchedFolders(); foldersRes) {
            for (const auto& wf : foldersRes.value()) {
                const QString fullPath = QString::fromStdWString(
                    mavira::data::fs::FileManager::stripWin32LongPathPrefix(wf.path.native()));
                QString displayName = QString::fromStdWString(wf.path.filename().native());
                if (displayName.isEmpty()) {
                    displayName = fullPath;
                }
                const bool online = mavira::data::fs::FileManager::isVolumeOnline(wf.path) &&
                                    mavira::data::fs::FileManager::isPathOnline(wf.path);
                const QString label = online
                    ? displayName
                    : QStringLiteral("%1 [OFFLINE]").arg(displayName);

                auto* folderNode = new QTreeWidgetItem(foldersRoot, QStringList{label});
                folderNode->setToolTip(0, fullPath);
                configureNodeItem(folderNode, SidebarNodeKind::WatchedFolder, wf.id, fullPath);
                folderNode->setData(0, kRolePopulated, false);
                folderNode->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            }
        }
    }
    foldersRoot->setExpanded(true);

    auto* seriesRoot = new QTreeWidgetItem(browserHeader, QStringList{LocalizationManager::tr("side.series_az")});
    configureNodeItem(seriesRoot, SidebarNodeKind::SectionHeader);

    if (db_ != nullptr && db_->isOpen()) {
        if (auto seriesRes = db_->getDistinctSeries(); seriesRes) {
            std::map<QChar, std::vector<std::string>> buckets;
            for (const auto& s : seriesRes.value()) {
                buckets[seriesBucketLetter(s)].push_back(s);
            }
            for (const auto& [letter, items] : buckets) {
                auto* bucketItem = new QTreeWidgetItem(
                    seriesRoot,
                    QStringList{QStringLiteral("%1 (%2)").arg(letter).arg(items.size())});
                configureNodeItem(
                    bucketItem,
                    SidebarNodeKind::SeriesBucket,
                    0,
                    QString(letter));

                for (const auto& seriesName : items) {
                    const QString qSeries = QString::fromStdString(seriesName);
                    auto* sItem = new QTreeWidgetItem(bucketItem, QStringList{qSeries});
                    configureNodeItem(sItem, SidebarNodeKind::SeriesItem, 0, qSeries);
                }
            }
        }
    }
    seriesRoot->setExpanded(true);
    browserHeader->setExpanded(true);

    auto attachSectionPlusButton = [this](
        QTreeWidgetItem* headerItem,
        const QString& title,
        const QString& tooltip,
        auto slot) {
        auto* rowWidget = new QWidget(tree_);
        rowWidget->setObjectName(QStringLiteral("SidebarSectionHeaderRow"));
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 4, 0);
        rowLayout->setSpacing(4);

        auto* lbl = new QLabel(title, rowWidget);
        lbl->setObjectName(QStringLiteral("SidebarSectionTitle"));
        auto* btnPlus = new QPushButton(QStringLiteral("+"), rowWidget);
        btnPlus->setObjectName(QStringLiteral("SidebarInlinePlusBtn"));
        btnPlus->setFixedSize(20, 18);
        btnPlus->setToolTip(tooltip);
        connect(btnPlus, &QPushButton::clicked, this, slot);

        rowLayout->addWidget(lbl, 1);
        rowLayout->addWidget(btnPlus);
        headerItem->setText(0, QString());
        tree_->setItemWidget(headerItem, 0, rowWidget);
    };

    // -------------------------------------------------------------------------
    // 3. SHELVES (Custom user collections)
    // -------------------------------------------------------------------------
    auto* shelvesHeader = new QTreeWidgetItem(tree_, QStringList{LocalizationManager::tr("side.shelves")});
    configureNodeItem(shelvesHeader, SidebarNodeKind::SectionHeader);
    shelvesHeader->setFlags(Qt::ItemIsEnabled);
    attachSectionPlusButton(
        shelvesHeader,
        LocalizationManager::tr("side.shelves"),
        QStringLiteral("Create a custom shelf"),
        &ExplorerSidebar::promptCreateShelf);

    for (const auto& shelf : allShelves) {
        if (shelf.name == kFavoritesShelfName) {
            continue; // Shown in top Library section
        }
        const QString qName = QString::fromStdString(shelf.name);
        auto* shelfItem = new QTreeWidgetItem(shelvesHeader, QStringList{qName});
        configureNodeItem(shelfItem, SidebarNodeKind::ShelfItem, shelf.id, qName);
    }
    shelvesHeader->setExpanded(true);

    // -------------------------------------------------------------------------
    // 4. SMARTVIEWS (Built-in Presets + Custom Rule Builder Smartviews)
    // -------------------------------------------------------------------------
    auto* smartViewsHeader = new QTreeWidgetItem(tree_, QStringList{LocalizationManager::tr("side.smartviews")});
    configureNodeItem(smartViewsHeader, SidebarNodeKind::SectionHeader);
    smartViewsHeader->setFlags(Qt::ItemIsEnabled);
    attachSectionPlusButton(
        smartViewsHeader,
        LocalizationManager::tr("side.smartviews"),
        QStringLiteral("Create a custom Smartview rule"),
        &ExplorerSidebar::openSmartViewBuilderModal);

    struct PresetDef {
        const char* label;
        const char* json;
    };
    constexpr PresetDef kPresets[] = {
        {"Unread Volumes", R"({"preset":"unread"})"},
        {"In Progress", R"({"preset":"in_progress"})"},
        {"Completed", R"({"preset":"completed"})"},
        {"Offline Pinned", R"({"preset":"offline_pinned"})"},
        {"Large Volumes (>100 MB)", R"({"preset":"large_volumes"})"}
    };

    for (const auto& preset : kPresets) {
        auto* pItem = new QTreeWidgetItem(
            smartViewsHeader, QStringList{QString::fromUtf8(preset.label)});
        configureNodeItem(
            pItem,
            SidebarNodeKind::SmartViewPreset,
            0,
            QString::fromUtf8(preset.json));
    }

    if (db_ != nullptr && db_->isOpen()) {
        if (auto svRes = db_->getSmartViews(); svRes) {
            for (const auto& sv : svRes.value()) {
                const QString qName = QString::fromStdString(sv.name);
                const QString qJson = QString::fromStdString(sv.queryJson);
                auto* svItem = new QTreeWidgetItem(smartViewsHeader, QStringList{qName});
                configureNodeItem(svItem, SidebarNodeKind::SmartViewCustom, sv.id, qJson);
            }
        }
    }
    smartViewsHeader->setExpanded(false);

    if (currentSelection_.label.isEmpty() && allComicsItem_ != nullptr) {
        tree_->setCurrentItem(allComicsItem_);
        currentSelection_.kind = SidebarNodeKind::AllComics;
        currentSelection_.recordId = 0;
        currentSelection_.label = LocalizationManager::tr("side.all_comics");
        currentSelection_.payload = QStringLiteral("ALL");
    }
}

void ExplorerSidebar::retranslateUi() {
    refreshNavigation();
}

void ExplorerSidebar::selectAllComics() {
    if (allComicsItem_ != nullptr) {
        tree_->setCurrentItem(allComicsItem_);
        onItemClicked(allComicsItem_, 0);
    }
}

void ExplorerSidebar::selectFavorites() {
    if (favoritesItem_ != nullptr) {
        tree_->setCurrentItem(favoritesItem_);
        onItemClicked(favoritesItem_, 0);
    }
}

void ExplorerSidebar::onItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }

    const SidebarNodeKind kind = itemNodeKind(item);
    if (kind == SidebarNodeKind::SectionHeader) {
        return;
    }
    if (kind == SidebarNodeKind::ToolItem) {
        emit toolRequested(item->data(0, kRolePayload).toString());
        return;
    }

    SidebarFilterSelection sel;
    sel.kind = kind;
    sel.recordId = item->data(0, kRoleRecordId).toLongLong();
    sel.label = item->text(0);
    sel.payload = item->data(0, kRolePayload).toString();

    currentSelection_ = sel;
    emit navigationSelected(sel);
}

void ExplorerSidebar::onItemExpanded(QTreeWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const SidebarNodeKind kind = itemNodeKind(item);
    if (kind != SidebarNodeKind::WatchedFolder && kind != SidebarNodeKind::ChildFolder) {
        return;
    }

    if (item->data(0, kRolePopulated).toBool()) {
        return;
    }

    const QString folderPathStr = item->data(0, kRolePayload).toString();
    populateFolderChildren(item, std::filesystem::path(folderPathStr.toStdWString()));
    item->setData(0, kRolePopulated, true);
}

void ExplorerSidebar::populateFolderChildren(
    QTreeWidgetItem* folderItem,
    const std::filesystem::path& folderPath) {
    if (db_ == nullptr || !db_->isOpen() || folderItem == nullptr) {
        return;
    }

    auto childrenRes = db_->getChildFolders(folderPath);
    if (!childrenRes) {
        return;
    }

    for (const auto& childPath : childrenRes.value()) {
        const QString fullPath = QString::fromStdWString(
            mavira::data::fs::FileManager::stripWin32LongPathPrefix(childPath.native()));
        QString name = QString::fromStdWString(childPath.filename().native());
        if (name.isEmpty()) {
            name = fullPath;
        }
        auto* childItem = new QTreeWidgetItem(folderItem, QStringList{name});
        childItem->setToolTip(0, fullPath);
        configureNodeItem(childItem, SidebarNodeKind::ChildFolder, 0, fullPath);
        childItem->setData(0, kRolePopulated, false);
        childItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

std::int64_t ExplorerSidebar::createCustomShelfByName(const QString& shelfName) {
    const QString trimmed = shelfName.trimmed();
    if (trimmed.isEmpty() || db_ == nullptr || !db_->isOpen()) {
        return 0;
    }
    auto res = db_->createShelf(trimmed.toStdString(), 0, nowUnix());
    if (!res) {
        return 0;
    }
    refreshNavigation();
    return res.value();
}

bool ExplorerSidebar::renameCustomShelfById(std::int64_t shelfId, const QString& newName) {
    const QString trimmed = newName.trimmed();
    if (shelfId <= 0 || trimmed.isEmpty() || db_ == nullptr || !db_->isOpen()) {
        return false;
    }
    auto res = db_->renameShelf(shelfId, trimmed.toStdString());
    if (!res) {
        return false;
    }
    refreshNavigation();
    return true;
}

bool ExplorerSidebar::deleteCustomShelfById(std::int64_t shelfId) {
    if (shelfId <= 0 || db_ == nullptr || !db_->isOpen()) {
        return false;
    }
    auto res = db_->deleteShelf(shelfId);
    if (!res) {
        return false;
    }
    refreshNavigation();
    return true;
}

std::int64_t ExplorerSidebar::createCustomSmartView(
    const QString& name,
    const QString& queryJson) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || db_ == nullptr || !db_->isOpen()) {
        return 0;
    }
    auto res = db_->createSmartView(trimmed.toStdString(), queryJson.toStdString(), 0, nowUnix());
    if (!res) {
        return 0;
    }
    refreshNavigation();
    return res.value();
}

bool ExplorerSidebar::deleteCustomSmartViewById(std::int64_t smartViewId) {
    if (smartViewId <= 0 || db_ == nullptr || !db_->isOpen()) {
        return false;
    }
    auto res = db_->deleteSmartView(smartViewId);
    if (!res) {
        return false;
    }
    refreshNavigation();
    return true;
}

void ExplorerSidebar::promptCreateShelf() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        QStringLiteral("New Shelf"),
        QStringLiteral("Shelf Name:"),
        QLineEdit::Normal,
        QString(),
        &ok);
    if (ok && !name.trimmed().isEmpty()) {
        (void)createCustomShelfByName(name);
    }
}

void ExplorerSidebar::promptRenameSelectedShelf() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr || itemNodeKind(item) != SidebarNodeKind::ShelfItem) {
        return;
    }

    const std::int64_t shelfId = item->data(0, kRoleRecordId).toLongLong();
    const QString currentName = item->text(0);

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this,
        QStringLiteral("Rename Shelf"),
        QStringLiteral("New Shelf Name:"),
        QLineEdit::Normal,
        currentName,
        &ok);
    if (ok && !newName.trimmed().isEmpty()) {
        (void)renameCustomShelfById(shelfId, newName);
    }
}

void ExplorerSidebar::promptDeleteSelectedItem() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) {
        return;
    }

    const SidebarNodeKind kind = itemNodeKind(item);
    const std::int64_t id = item->data(0, kRoleRecordId).toLongLong();

    if (kind == SidebarNodeKind::ShelfItem) {
        (void)deleteCustomShelfById(id);
    } else if (kind == SidebarNodeKind::SmartViewCustom) {
        (void)deleteCustomSmartViewById(id);
    } else if (kind == SidebarNodeKind::WatchedFolder && libraryManager_ != nullptr) {
        (void)libraryManager_->removeWatchedFolder(id);
        refreshNavigation();
    }
}

void ExplorerSidebar::openSmartViewBuilderModal() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Smartview Rule Builder"));
    dlg.setModal(true);
    dlg.resize(440, 240);

    auto* layout = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();

    auto* nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(QStringLiteral("e.g., Cyberpunk Manga"));

    auto* fieldCombo = new QComboBox(&dlg);
    fieldCombo->addItem(QStringLiteral("Title"), QStringLiteral("title"));
    fieldCombo->addItem(QStringLiteral("Series"), QStringLiteral("series"));
    fieldCombo->addItem(QStringLiteral("Author"), QStringLiteral("author"));
    fieldCombo->addItem(QStringLiteral("Tags"), QStringLiteral("tags"));
    fieldCombo->addItem(QStringLiteral("Read Status"), QStringLiteral("is_read"));

    auto* opCombo = new QComboBox(&dlg);
    opCombo->addItem(QStringLiteral("Contains"), QStringLiteral("contains"));
    opCombo->addItem(QStringLiteral("Equals"), QStringLiteral("equals"));
    opCombo->addItem(QStringLiteral("Starts With"), QStringLiteral("starts_with"));

    auto* valueEdit = new QLineEdit(&dlg);
    valueEdit->setPlaceholderText(QStringLiteral("Filter value..."));

    form->addRow(QStringLiteral("Smartview Name:"), nameEdit);
    form->addRow(QStringLiteral("Match Field:"), fieldCombo);
    form->addRow(QStringLiteral("Condition:"), opCombo);
    form->addRow(QStringLiteral("Value:"), valueEdit);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        const QString svName = nameEdit->text().trimmed();
        if (svName.isEmpty()) {
            return;
        }
        QJsonObject ruleObj;
        ruleObj.insert(QStringLiteral("field"), fieldCombo->currentData().toString());
        ruleObj.insert(QStringLiteral("operator"), opCombo->currentData().toString());
        ruleObj.insert(QStringLiteral("value"), valueEdit->text().trimmed());

        QJsonArray rulesArray;
        rulesArray.append(ruleObj);

        QJsonObject rootObj;
        rootObj.insert(QStringLiteral("rules"), rulesArray);

        const QString jsonStr = QString::fromUtf8(
            QJsonDocument(rootObj).toJson(QJsonDocument::Compact));
        (void)createCustomSmartView(svName, jsonStr);
    }
}

void ExplorerSidebar::onCustomContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (item == nullptr) {
        return;
    }
    tree_->setCurrentItem(item);
    const SidebarNodeKind kind = itemNodeKind(item);

    QMenu menu(this);
    if (kind == SidebarNodeKind::ShelfItem) {
        menu.addAction(QStringLiteral("Rename Shelf..."), this, &ExplorerSidebar::promptRenameSelectedShelf);
        menu.addAction(QStringLiteral("Delete Shelf"), this, &ExplorerSidebar::promptDeleteSelectedItem);
    } else if (kind == SidebarNodeKind::SmartViewCustom) {
        menu.addAction(QStringLiteral("New Smartview..."), this, &ExplorerSidebar::openSmartViewBuilderModal);
        menu.addAction(QStringLiteral("Delete Smartview"), this, &ExplorerSidebar::promptDeleteSelectedItem);
    } else if (kind == SidebarNodeKind::CloudSource) {
        const QString cloudPath = item->data(0, kRolePayload).toString();
        menu.addAction(QStringLiteral("Add Cloud Folder to Watched Library"), this, [this, cloudPath]() {
            emit cloudFolderWatchRequested(cloudPath);
        });
    } else if (kind == SidebarNodeKind::WatchedFolder) {
        menu.addAction(QStringLiteral("Remove Watched Folder"), this, &ExplorerSidebar::promptDeleteSelectedItem);
    } else {
        menu.addAction(QStringLiteral("New Shelf..."), this, &ExplorerSidebar::promptCreateShelf);
        menu.addAction(QStringLiteral("New Smartview..."), this, &ExplorerSidebar::openSmartViewBuilderModal);
    }

    if (!menu.isEmpty()) {
        menu.exec(tree_->viewport()->mapToGlobal(pos));
    }
}

} // namespace mavira::ui
