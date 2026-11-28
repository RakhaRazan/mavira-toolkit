#include "ui/MainWindow.hpp"

#include "data/fs/FileManager.hpp"
#include "ui/LocalizationManager.hpp"
#include "ui/MergeComicsDialog.hpp"
#include "ui/PreferencesWorkspace.hpp"
#include "ui/SettingsDialog.hpp"
#include "ui/ToolWorkspaceWidget.hpp"
#include "ui/ToolsDialogs.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace mavira::ui {

namespace {

[[nodiscard]] QString displayTitleForComic(const mavira::data::db::ComicRecord& c) {
    if (!c.title.empty()) {
        return QString::fromStdString(c.title);
    }
    return QString::fromStdWString(c.filePath.stem().native());
}

[[nodiscard]] bool matchesStringRule(
    const QString& fieldValue,
    const QString& op,
    const QString& targetValue) {
    if (op == QStringLiteral("equals")) {
        return fieldValue.compare(targetValue, Qt::CaseInsensitive) == 0;
    }
    if (op == QStringLiteral("starts_with")) {
        return fieldValue.startsWith(targetValue, Qt::CaseInsensitive);
    }
    return fieldValue.contains(targetValue, Qt::CaseInsensitive);
}

[[nodiscard]] QString resolveAssetPath(const wchar_t* relativePath) {
    const std::filesystem::path exeDir =
        mavira::data::fs::FileManager::getExecutableDirectory();
    const std::vector<std::filesystem::path> candidates = {
        exeDir / relativePath,
        exeDir.parent_path() / relativePath,
        exeDir.parent_path().parent_path() / relativePath
    };
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return QString::fromStdWString(candidate.native());
        }
    }
    return {};
}

[[nodiscard]] QIcon loadApplicationIcon() {
    if (const QString icoPath = resolveAssetPath(L"assets/app.ico"); !icoPath.isEmpty()) {
        QIcon icon(icoPath);
        if (!icon.pixmap(32, 32).isNull()) {
            return icon;
        }
    }
    if (const QString svgPath = resolveAssetPath(L"assets/logo.svg"); !svgPath.isEmpty()) {
        QIcon icon(svgPath);
        if (!icon.pixmap(32, 32).isNull()) {
            return icon;
        }
    }
#ifdef _WIN32
    QIcon winIcon;
    for (const int sz : {16, 24, 32, 48, 64, 128, 256}) {
        HICON hIcon = static_cast<HICON>(::LoadImageW(
            ::GetModuleHandleW(nullptr),
            L"IDI_ICON1",
            IMAGE_ICON,
            sz,
            sz,
            LR_DEFAULTCOLOR));
        if (hIcon != nullptr) {
            const QImage img = QImage::fromHICON(hIcon);
            ::DestroyIcon(hIcon);
            if (!img.isNull()) {
                winIcon.addPixmap(QPixmap::fromImage(img));
            }
        }
    }
    if (!winIcon.isNull()) {
        return winIcon;
    }
#endif
    return {};
}

} // namespace

MainWindow::MainWindow(
    mavira::data::db::DatabaseManager* db,
    mavira::core::cache::CacheManager* cacheManager,
    mavira::app::LibraryManager* libraryManager,
    mavira::data::config::SettingsManager* settingsManager,
    QWidget* parent)
    : QMainWindow(parent),
      db_(db),
      cacheManager_(cacheManager),
      libraryManager_(libraryManager),
      settingsManager_(settingsManager) {
    setWindowTitle(QStringLiteral("MAVIRA - Batch Comic Converter, Extractor, Merger & Offline OCR Suite"));
    baseAppFont_ = QApplication::font();
    if (baseAppFont_.pointSizeF() <= 0.0) {
        baseAppFont_.setPointSizeF(9.5);
    }
    if (const QIcon appIcon = loadApplicationIcon(); !appIcon.isNull()) {
        setWindowIcon(appIcon);
    }
    resize(1360, 840);

    appCore_ = new mavira::app::ApplicationCore(db_, this);
    readerController_ = new mavira::app::ReaderController(db_, cacheManager_, nullptr, this);

    if (settingsManager_ != nullptr) {
        if (auto loadRes = settingsManager_->load(); loadRes) {
            settings_ = loadRes.value();
            currentTheme_ = settings_.theme.empty() ? "obsidian_cyber" : settings_.theme;
        }
    }

    setupUi();
    applySettings(settings_, /*persist=*/false);
    refreshLibraryView();

    if (libraryManager_ != nullptr) {
        connect(
            libraryManager_,
            &mavira::app::LibraryManager::aboutToDeleteComic,
            this,
            [this](std::int64_t comicId, const std::wstring& filePath) {
                if (readerController_ != nullptr) {
                    (void)readerController_->closeIfMatching(comicId, filePath);
                }
            },
            Qt::DirectConnection);
        connect(
            libraryManager_,
            &mavira::app::LibraryManager::comicRemoved,
            this,
            [this](std::int64_t comicId, const std::wstring& filePath) {
                if (readerController_ != nullptr) {
                    (void)readerController_->closeIfMatching(comicId, filePath);
                }
            });
        connect(
            libraryManager_,
            &mavira::app::LibraryManager::libraryUpdated,
            this,
            [this]() {
                if (sidebar_ != nullptr) {
                    sidebar_->refreshNavigation();
                }
                refreshLibraryView();
            });
        connect(
            libraryManager_,
            &mavira::app::LibraryManager::scanProgress,
            this,
            &MainWindow::onScanProgress);
        connect(
            libraryManager_,
            &mavira::app::LibraryManager::scanFinished,
            this,
            &MainWindow::onScanFinished);
    }
}

MainWindow::~MainWindow() {
    if (appCore_ != nullptr) {
        appCore_->setKeepScreenActive(false);
    }
    if (readerController_ != nullptr) {
        readerController_->shutdown();
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event != nullptr &&
        (event->type() == QEvent::WindowStateChange || event->type() == QEvent::ActivationChange)) {
        updateScreenKeepAlive();
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (appCore_ != nullptr) {
        appCore_->setKeepScreenActive(false);
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::updateScreenKeepAlive() {
    if (appCore_ == nullptr) {
        return;
    }
    const bool shouldKeepActive =
        (activeViewMode_ == ActiveViewMode::Reader) &&
        settings_.keep_screen_active &&
        !isMinimized();
    appCore_->setKeepScreenActive(shouldKeepActive);
}

void MainWindow::refreshTaskbarJumpList() {
    if (appCore_ != nullptr && db_ != nullptr && db_->isOpen()) {
        (void)appCore_->refreshJumpListFromDatabase(10, settings_.show_hidden_items);
    }
}

void MainWindow::setupMenuBar() {
    QMenuBar* mb = menuBar();
    mb->setObjectName(QStringLiteral("MainMenuBar"));

    // File
    fileMenu_ = mb->addMenu(LocalizationManager::tr("menu.file"));
    fileMenu_->addAction(LocalizationManager::tr("menu.file.open_file"), QKeySequence::Open, this, &MainWindow::promptOpenComicFile);
    fileMenu_->addAction(LocalizationManager::tr("menu.file.add_folder"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), this, &MainWindow::promptAddWatchedFolder);
    fileMenu_->addAction(LocalizationManager::tr("menu.file.prune_unavailable"), this, [this]() {
        if (libraryManager_ != nullptr) {
            libraryManager_->pruneOrphansAsync();
            if (statusLabel_ != nullptr) {
                statusLabel_->setText(QStringLiteral("Scanning for unavailable library entries..."));
            }
        }
    });
    fileMenu_->addSeparator();
    fileMenu_->addAction(LocalizationManager::tr("menu.file.exit"), QKeySequence::Quit, this, &QWidget::close);

    // Edit
    editMenu_ = mb->addMenu(LocalizationManager::tr("menu.edit"));
    editMenu_->addAction(LocalizationManager::tr("menu.edit.select_all"), QKeySequence::SelectAll, this, [this]() {
        if (libraryView_ != nullptr) {
            libraryView_->selectAll();
        }
    });
    editMenu_->addAction(LocalizationManager::tr("menu.edit.remove_selected"), QKeySequence::Delete, this, [this]() {
        if (libraryView_ != nullptr) {
            libraryView_->removeSelectedFromLibrary(false);
        }
    });
    editMenu_->addAction(LocalizationManager::tr("menu.edit.delete_selected"), QKeySequence(QStringLiteral("Shift+Del")), this, [this]() {
        if (libraryView_ != nullptr) {
            libraryView_->removeSelectedFromLibrary(true);
        }
    });
    editMenu_->addSeparator();
    editMenu_->addAction(LocalizationManager::tr("menu.edit.preferences"), QKeySequence(QStringLiteral("Ctrl+,")), this, &MainWindow::openSettingsDialog);

    // Tools
    toolsMenu_ = mb->addMenu(LocalizationManager::tr("menu.tools"));

    QMenu* convertMenu = toolsMenu_->addMenu(LocalizationManager::tr("menu.tools.convert"));
    convertMenu->addAction(LocalizationManager::tr("menu.tools.convert_comics"), this, &MainWindow::openConvertComicDialog);
    convertMenu->addAction(LocalizationManager::tr("menu.tools.convert_images"), this, &MainWindow::openBatchConvertImagesDialog);

    QMenu* createMenu = toolsMenu_->addMenu(LocalizationManager::tr("menu.tools.create"));
    createMenu->addAction(LocalizationManager::tr("menu.tools.create_comic"), this, &MainWindow::openCreateComicDialog);

    QMenu* extractMenu = toolsMenu_->addMenu(LocalizationManager::tr("menu.tools.extract"));
    extractMenu->addAction(LocalizationManager::tr("menu.tools.extract_pages"), this, &MainWindow::openExtractPagesDialog);
    extractMenu->addAction(LocalizationManager::tr("menu.tools.extract_ocr"), this, &MainWindow::openNativeOcrDialog);

    QMenu* mergeMenu = toolsMenu_->addMenu(LocalizationManager::tr("menu.tools.merge"));
    mergeMenu->addAction(LocalizationManager::tr("menu.tools.merge_comics"), this, [this]() {
        openMergeComicsDialog();
    });

    // View
    viewMenu_ = mb->addMenu(LocalizationManager::tr("menu.view"));
    viewMenu_->addAction(LocalizationManager::tr("menu.view.library_grid"), this, [this]() {
        switchToView(ActiveViewMode::Library);
    });
    viewMenu_->addAction(LocalizationManager::tr("menu.view.sidebar_browser"), this, [this]() {
        if (sidebar_ != nullptr) {
            sidebar_->setVisible(!sidebar_->isVisible());
        }
    });
    viewMenu_->addAction(LocalizationManager::tr("menu.view.reader_view"), this, [this]() {
        switchToView(ActiveViewMode::Reader);
    });
    viewMenu_->addSeparator();
    viewMenu_->addAction(LocalizationManager::tr("menu.view.toggle_theme"), this, &MainWindow::toggleTheme);

    // Help
    helpMenu_ = mb->addMenu(LocalizationManager::tr("menu.help"));
    helpMenu_->addAction(LocalizationManager::tr("menu.help.shortcuts"), this, &MainWindow::openShortcutsDialog);
    helpMenu_->addAction(LocalizationManager::tr("menu.help.about"), this, &MainWindow::openAboutDialog);
}

void MainWindow::setupUi() {
    setupMenuBar();

    auto* rootWidget = new QWidget(this);
    rootWidget->setObjectName(QStringLiteral("MainShellRoot"));
    auto* rootLayout = new QVBoxLayout(rootWidget);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // -------------------------------------------------------------------------
    // 1. Rounded Top Action Bar
    // -------------------------------------------------------------------------
    auto* topBar = new QWidget(rootWidget);
    topBar->setObjectName(QStringLiteral("TopCommandBar"));
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(12, 8, 12, 8);
    topLayout->setSpacing(8);

    btnAddFolder_ = new QPushButton(QStringLiteral("Add Folder"), topBar);
    btnAddFolder_->setObjectName(QStringLiteral("BtnAddFolder"));

    btnOpenFile_ = new QPushButton(QStringLiteral("Open File"), topBar);
    btnOpenFile_->setObjectName(QStringLiteral("BtnOpenFile"));

    btnRandomUnread_ = new QPushButton(QStringLiteral("Random Unread"), topBar);
    btnRandomUnread_->setObjectName(QStringLiteral("BtnRandomUnread"));

    searchEdit_ = new QLineEdit(topBar);
    searchEdit_->setObjectName(QStringLiteral("FtsSearchBar"));
    searchEdit_->setPlaceholderText(
        QStringLiteral("Search library (title, series, author, tags, path)..."));
    searchEdit_->setClearButtonEnabled(true);

    cmbSortBy_ = new QComboBox(topBar);
    cmbSortBy_->setObjectName(QStringLiteral("CmbSortBy"));
    cmbSortBy_->addItem(LocalizationManager::tr("sort.title_az"), QStringLiteral("title_az"));
    cmbSortBy_->addItem(LocalizationManager::tr("sort.recently_added"), QStringLiteral("added_desc"));
    cmbSortBy_->addItem(LocalizationManager::tr("sort.last_read"), QStringLiteral("last_read_desc"));
    cmbSortBy_->addItem(LocalizationManager::tr("sort.file_size"), QStringLiteral("size_desc"));

    cmbStatusFilter_ = new QComboBox(topBar);
    cmbStatusFilter_->setObjectName(QStringLiteral("CmbStatusFilter"));
    cmbStatusFilter_->addItem(LocalizationManager::tr("filter.all"), QStringLiteral("all"));
    cmbStatusFilter_->addItem(LocalizationManager::tr("filter.unread"), QStringLiteral("unread"));
    cmbStatusFilter_->addItem(LocalizationManager::tr("filter.in_progress"), QStringLiteral("in_progress"));
    cmbStatusFilter_->addItem(LocalizationManager::tr("filter.finished"), QStringLiteral("finished"));

    btnLibraryView_ = new QPushButton(QStringLiteral("Library"), topBar);
    btnLibraryView_->setObjectName(QStringLiteral("BtnLibraryView"));
    btnLibraryView_->setCheckable(true);
    btnLibraryView_->setChecked(true);

    btnReaderView_ = new QPushButton(QStringLiteral("Reader"), topBar);
    btnReaderView_->setObjectName(QStringLiteral("BtnReaderView"));
    btnReaderView_->setCheckable(true);
    btnReaderView_->setChecked(false);

    btnSettings_ = new QPushButton(QStringLiteral("Settings"), topBar);
    btnSettings_->setObjectName(QStringLiteral("BtnSettings"));

    topLayout->addWidget(btnAddFolder_);
    topLayout->addWidget(btnOpenFile_);
    topLayout->addWidget(btnRandomUnread_);
    topLayout->addWidget(searchEdit_, 1);
    topLayout->addWidget(cmbSortBy_);
    topLayout->addWidget(cmbStatusFilter_);
    topLayout->addWidget(btnLibraryView_);
    topLayout->addWidget(btnReaderView_);
    topLayout->addWidget(btnSettings_);

    // -------------------------------------------------------------------------
    // 2. Main Splitter: ExplorerSidebar (Left) + Central View Stack (Right)
    // -------------------------------------------------------------------------
    mainSplitter_ = new QSplitter(Qt::Horizontal, rootWidget);
    mainSplitter_->setObjectName(QStringLiteral("MainSplitter"));
    mainSplitter_->setChildrenCollapsible(false);
    mainSplitter_->setHandleWidth(1);

    sidebar_ = new ExplorerSidebar(db_, libraryManager_, mainSplitter_);
    sidebar_->setMinimumWidth(230);
    sidebar_->setMaximumWidth(380);

    centralStack_ = new QStackedWidget(mainSplitter_);
    centralStack_->setObjectName(QStringLiteral("CentralViewStack"));

    // --- Page 0: Library View (Continue Reading Strip + High-Density Table) ---
    libraryPage_ = new QWidget(centralStack_);
    libraryPage_->setObjectName(QStringLiteral("LibraryPage"));
    auto* libLayout = new QVBoxLayout(libraryPage_);
    libLayout->setContentsMargins(0, 0, 0, 0);
    libLayout->setSpacing(0);

    continueReadingContainer_ = new QWidget(libraryPage_);
    continueReadingContainer_->setObjectName(QStringLiteral("ContinueReadingStrip"));
    auto* crLayout = new QVBoxLayout(continueReadingContainer_);
    crLayout->setContentsMargins(10, 8, 10, 8);
    crLayout->setSpacing(4);

    continueReadingTitleLabel_ = new QLabel(LocalizationManager::tr("strip.continue_reading"), continueReadingContainer_);
    continueReadingTitleLabel_->setObjectName(QStringLiteral("SectionStripHeader"));

    continueReadingList_ = new QListWidget(continueReadingContainer_);
    continueReadingList_->setObjectName(QStringLiteral("ContinueReadingList"));
    continueReadingList_->setFlow(QListView::LeftToRight);
    continueReadingList_->setWrapping(false);
    continueReadingList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    continueReadingList_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    continueReadingList_->setFixedHeight(68);
    continueReadingList_->setSelectionMode(QAbstractItemView::SingleSelection);

    crLayout->addWidget(continueReadingTitleLabel_);
    crLayout->addWidget(continueReadingList_);

    viewHeaderLabel_ = new QLabel(QStringLiteral("ALL COMICS"), libraryPage_);
    viewHeaderLabel_->setObjectName(QStringLiteral("ViewHeaderLabel"));

    libraryView_ = new LibraryView(db_, cacheManager_, libraryManager_, libraryPage_);

    libLayout->addWidget(continueReadingContainer_);
    libLayout->addWidget(viewHeaderLabel_);
    libLayout->addWidget(libraryView_, 1);

    // --- Page 1: GPU-Accelerated Reader View (ReaderViewport lazy-instantiated on first open) ---
    readerPage_ = new QWidget(centralStack_);
    readerPage_->setObjectName(QStringLiteral("ReaderPage"));
    readerLayout_ = new QVBoxLayout(readerPage_);
    readerLayout_->setContentsMargins(0, 0, 0, 0);
    readerLayout_->setSpacing(0);

    auto* readerTopWidget = new QWidget(readerPage_);
    readerTopWidget->setObjectName(QStringLiteral("ReaderTopBar"));
    auto* readerTopBar = new QHBoxLayout(readerTopWidget);
    readerTopBar->setContentsMargins(8, 4, 8, 4);
    readerTopBar->setSpacing(8);

    btnBackToLibrary_ = new QPushButton(QStringLiteral("Back to Library (Esc)"), readerTopWidget);
    btnBackToLibrary_->setObjectName(QStringLiteral("BtnBackToLibrary"));
    readerTitleLabel_ = new QLabel(QStringLiteral("No Volume Loaded"), readerTopWidget);
    readerTitleLabel_->setObjectName(QStringLiteral("ReaderTitleLabel"));

    readerTopBar->addWidget(btnBackToLibrary_);
    readerTopBar->addWidget(readerTitleLabel_, 1);

    readerLayout_->addWidget(readerTopWidget);

    toolWorkspaceHost_ = new QWidget(centralStack_);
    auto* toolHostLayout = new QVBoxLayout(toolWorkspaceHost_);
    toolHostLayout->setContentsMargins(0, 0, 0, 0);
    toolHostLayout->setSpacing(0);

    prefsWorkspaceHost_ = new QWidget(centralStack_);
    auto* prefsHostLayout = new QVBoxLayout(prefsWorkspaceHost_);
    prefsHostLayout->setContentsMargins(0, 0, 0, 0);
    prefsHostLayout->setSpacing(0);

    centralStack_->addWidget(libraryPage_);
    centralStack_->addWidget(readerPage_);
    centralStack_->addWidget(toolWorkspaceHost_);
    centralStack_->addWidget(prefsWorkspaceHost_);

    mainSplitter_->addWidget(sidebar_);
    mainSplitter_->addWidget(centralStack_);
    mainSplitter_->setStretchFactor(0, 0);
    mainSplitter_->setStretchFactor(1, 1);
    mainSplitter_->setSizes({260, 1100});

    rootLayout->addWidget(topBar);
    rootLayout->addWidget(mainSplitter_, 1);
    setCentralWidget(rootWidget);

    // -------------------------------------------------------------------------
    // 3. Status Bar & Background Scan Progress
    // -------------------------------------------------------------------------
    statusLabel_ = new QLabel(QStringLiteral("Ready"), this);
    scanProgressBar_ = new QProgressBar(this);
    scanProgressBar_->setObjectName(QStringLiteral("ScanProgressBar"));
    scanProgressBar_->setMaximumWidth(220);
    scanProgressBar_->setFixedHeight(14);
    scanProgressBar_->setTextVisible(false);
    scanProgressBar_->setVisible(false);

    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addPermanentWidget(scanProgressBar_);

    // -------------------------------------------------------------------------
    // 4. Signal Wiring
    // -------------------------------------------------------------------------
    connect(btnAddFolder_, &QPushButton::clicked, this, &MainWindow::promptAddWatchedFolder);
    connect(btnOpenFile_, &QPushButton::clicked, this, &MainWindow::promptOpenComicFile);
    connect(btnRandomUnread_, &QPushButton::clicked, this, &MainWindow::triggerRandomUnread);
    connect(searchEdit_, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    connect(cmbSortBy_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        applySortAndStatusFilter();
    });
    connect(cmbStatusFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        applySortAndStatusFilter();
    });

    connect(btnLibraryView_, &QPushButton::clicked, this, [this]() {
        switchToView(ActiveViewMode::Library);
    });
    connect(btnReaderView_, &QPushButton::clicked, this, [this]() {
        switchToView(ActiveViewMode::Reader);
    });
    connect(btnBackToLibrary_, &QPushButton::clicked, this, [this]() {
        if (isFullScreen()) {
            showNormal();
        }
        switchToView(ActiveViewMode::Library);
    });

    auto* f11Shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(f11Shortcut, &QShortcut::activated, this, [this]() {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
    });
    if (readerController_ != nullptr) {
        connect(readerController_, &mavira::app::ReaderController::pageChanged, this,
                [this](int pageIndex, int totalPages) {
                    if (readerTitleLabel_ != nullptr && readerController_->isOpen()) {
                        const QString name = QString::fromStdWString(
                            readerController_->currentPath().stem().native());
                        readerTitleLabel_->setText(
                            QStringLiteral("%1 - Page %2 / %3")
                                .arg(name.isEmpty() ? QStringLiteral("Volume") : name)
                                .arg(pageIndex + 1)
                                .arg(std::max(1, totalPages)));
                    }
                });
        connect(readerController_, &mavira::app::ReaderController::readingProgressSaved, this,
                [this](qint64, int, bool) {
                    refreshContinueReadingStrip();
                    refreshTaskbarJumpList();
                });
        connect(readerController_, &mavira::app::ReaderController::errorOccurred, this,
                [this](const QString& msg) {
                    if (statusLabel_ != nullptr) {
                        statusLabel_->setText(msg);
                    }
                });
        connect(readerController_, &mavira::app::ReaderController::activeComicClosed, this,
                [this]() {
                    if (readerViewport_ != nullptr) {
                        readerViewport_->clearPage();
                    }
                    if (readerTitleLabel_ != nullptr) {
                        readerTitleLabel_->setText(LocalizationManager::tr("reader.no_volume"));
                    }
                    if (activeViewMode_ == ActiveViewMode::Reader) {
                        switchToView(ActiveViewMode::Library);
                    }
                });
    }
    connect(btnSettings_, &QPushButton::clicked, this, &MainWindow::openPreferencesWorkspace);

    connect(
        sidebar_,
        &ExplorerSidebar::navigationSelected,
        this,
        &MainWindow::onSidebarNavigationSelected);
    connect(
        sidebar_,
        &ExplorerSidebar::cloudFolderWatchRequested,
        this,
        [this](const QString& folderPath) {
            addFolderPathAsync(std::filesystem::path(folderPath.toStdWString()));
        });
    connect(
        sidebar_,
        &ExplorerSidebar::toolRequested,
        this,
        [this](const QString& toolId) {
            std::vector<std::filesystem::path> initialPaths;
            if (libraryView_ != nullptr) {
                for (const auto& rec : libraryView_->selectedComics()) {
                    if (!rec.filePath.empty()) {
                        initialPaths.push_back(rec.filePath);
                    }
                }
            }
            openToolWorkspace(toolId, initialPaths);
        });

    connect(
        continueReadingList_,
        &QListWidget::itemActivated,
        this,
        &MainWindow::onContinueReadingItemActivated);
    connect(
        continueReadingList_,
        &QListWidget::itemClicked,
        this,
        &MainWindow::onContinueReadingItemActivated);
    connect(
        libraryView_,
        &LibraryView::comicActivated,
        this,
        &MainWindow::openComicInReader);
    connect(
        libraryView_,
        &LibraryView::libraryMutated,
        this,
        [this]() {
            if (sidebar_ != nullptr) {
                sidebar_->refreshNavigation();
            }
            refreshLibraryView();
        });
    connect(
        libraryView_,
        &LibraryView::statusMessage,
        this,
        [this](const QString& msg) {
            if (statusLabel_ != nullptr) {
                statusLabel_->setText(msg);
            }
        });
    connect(
        libraryView_,
        &LibraryView::toolWorkspaceRequested,
        this,
        &MainWindow::openToolWorkspace);
}

ReaderViewport* MainWindow::ensureReaderViewport() {
    if (readerViewport_ != nullptr) {
        return readerViewport_;
    }
    readerViewport_ = new ReaderViewport(readerController_, readerPage_);
    readerViewport_->setObjectName(QStringLiteral("ReaderViewport"));
    if (readerLayout_ != nullptr) {
        readerLayout_->addWidget(readerViewport_, 1);
    }
    connect(readerViewport_, &ReaderViewport::backToLibraryRequested, this, [this]() {
        if (isFullScreen()) {
            showNormal();
        }
        switchToView(ActiveViewMode::Library);
    });
    connect(readerViewport_, &ReaderViewport::toggleFullscreenRequested, this, [this]() {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
    });
    readerViewport_->applySettings(settings_);
    readerViewport_->retranslateUi();
    return readerViewport_;
}

ToolWorkspaceWidget* MainWindow::ensureToolWorkspace() {
    if (toolWorkspace_ != nullptr) {
        return toolWorkspace_;
    }
    toolWorkspace_ = new ToolWorkspaceWidget(settingsManager_, toolWorkspaceHost_);
    if (toolWorkspaceHost_ != nullptr && toolWorkspaceHost_->layout() != nullptr) {
        toolWorkspaceHost_->layout()->addWidget(toolWorkspace_);
    }
    connect(toolWorkspace_, &ToolWorkspaceWidget::closeRequested, this, [this]() {
        switchToView(previousViewMode_);
    });
    connect(toolWorkspace_, &ToolWorkspaceWidget::statusMessage, this, [this](const QString& msg) {
        if (statusLabel_ != nullptr) {
            statusLabel_->setText(msg);
        }
    });
    connect(toolWorkspace_, &ToolWorkspaceWidget::toolCompleted, this,
            [this](const QString&, const QString& summary, bool mutatedLibrary) {
                if (statusLabel_ != nullptr) {
                    statusLabel_->setText(summary);
                }
                if (mutatedLibrary) {
                    refreshLibraryView();
                }
            });
    toolWorkspace_->retranslateUi();
    return toolWorkspace_;
}

PreferencesWorkspace* MainWindow::ensurePreferencesWorkspace() {
    if (prefsWorkspace_ != nullptr) {
        return prefsWorkspace_;
    }
    const auto portablePaths = (libraryManager_ != nullptr)
        ? libraryManager_->portablePaths()
        : mavira::data::fs::FileManager::resolvePortablePaths();
    prefsWorkspace_ = new PreferencesWorkspace(db_, cacheManager_, portablePaths, prefsWorkspaceHost_);
    if (prefsWorkspaceHost_ != nullptr && prefsWorkspaceHost_->layout() != nullptr) {
        prefsWorkspaceHost_->layout()->addWidget(prefsWorkspace_);
    }
    connect(prefsWorkspace_, &PreferencesWorkspace::backRequested, this, [this]() {
        switchToView(previousViewMode_);
    });
    connect(prefsWorkspace_, &PreferencesWorkspace::settingsApplied, this,
            [this](const mavira::data::config::AppSettings& s) {
                applySettings(s, /*persist=*/true);
            });
    connect(prefsWorkspace_, &PreferencesWorkspace::coverCachePurged, this, [this]() {
        if (libraryView_ != nullptr) {
            libraryView_->clearCoverCache();
        }
    });
    connect(prefsWorkspace_, &PreferencesWorkspace::coversPackBuilt, this, [this]() {
        if (libraryView_ != nullptr) {
            libraryView_->clearCoverCache();
        }
    });
    prefsWorkspace_->retranslateUi();
    return prefsWorkspace_;
}

ReaderViewport* MainWindow::readerViewport() {
    return ensureReaderViewport();
}

ToolWorkspaceWidget* MainWindow::toolWorkspace() {
    return ensureToolWorkspace();
}

PreferencesWorkspace* MainWindow::preferencesWorkspace() {
    return ensurePreferencesWorkspace();
}

namespace {

struct ThemePalette {
    std::string id;
    bool isDark;
    QString canvasStart;
    QString canvasEnd;
    QString surface;
    QString surfaceAlt;
    QString cardBg;
    QString cardHeaderBg;
    QString inputBg;
    QString border;
    QString hoverBorder;
    QString text;
    QString mutedText;
    QString accentStart;
    QString accentEnd;
    QString accentSolid;
    QString accentText;
    QString accentTintBg;
    QString btnBg;
    QString btnHoverBg;
    QString dangerBg;
    QString dangerText;
    QString dangerBorder;
};

[[nodiscard]] ThemePalette resolveThemePalette(std::string_view themeName) {
    if (themeName == "midnight_velvet") {
        return ThemePalette{
            "midnight_velvet", true,
            QStringLiteral("#0D0B16"), QStringLiteral("#181329"),
            QStringLiteral("#131020"), QStringLiteral("#171326"),
            QStringLiteral("#1D1730"), QStringLiteral("#251E3D"),
            QStringLiteral("#0F0C1A"), QStringLiteral("#31274D"), QStringLiteral("#A78BFA"),
            QStringLiteral("#F3E8FF"), QStringLiteral("#C4B5FD"),
            QStringLiteral("#A78BFA"), QStringLiteral("#7C3AED"), QStringLiteral("#A78BFA"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#271E45"),
            QStringLiteral("#221B38"), QStringLiteral("#2D244A"),
            QStringLiteral("#450A0A"), QStringLiteral("#FCA5A5"), QStringLiteral("#7F1D1D")
        };
    }
    if (themeName == "warm_parchment") {
        return ThemePalette{
            "warm_parchment", false,
            QStringLiteral("#F5EFE4"), QStringLiteral("#EAE0CE"),
            QStringLiteral("#F9F4EA"), QStringLiteral("#F2EADF"),
            QStringLiteral("#FCF9F2"), QStringLiteral("#EFE6D5"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#D6C8B2"), QStringLiteral("#D97706"),
            QStringLiteral("#292118"), QStringLiteral("#6B5B47"),
            QStringLiteral("#D97706"), QStringLiteral("#B45309"), QStringLiteral("#B45309"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#FEF3C7"),
            QStringLiteral("#FBF7EE"), QStringLiteral("#F3E9D8"),
            QStringLiteral("#FEE2E2"), QStringLiteral("#991B1B"), QStringLiteral("#FCA5A5")
        };
    }
    if (themeName == "nordic_glacier" || themeName == "light" || themeName == "Light") {
        return ThemePalette{
            "nordic_glacier", false,
            QStringLiteral("#E7EFF8"), QStringLiteral("#D8E4F2"),
            QStringLiteral("#F2F7FC"), QStringLiteral("#EAF2FA"),
            QStringLiteral("#F7FBFF"), QStringLiteral("#E1ECF7"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#BDD0E4"), QStringLiteral("#0284C7"),
            QStringLiteral("#0F172A"), QStringLiteral("#475569"),
            QStringLiteral("#0284C7"), QStringLiteral("#0369A1"), QStringLiteral("#0284C7"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#E0F2FE"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#EBF4FC"),
            QStringLiteral("#FEE2E2"), QStringLiteral("#B91C1C"), QStringLiteral("#FCA5A5")
        };
    }
    if (themeName == "sakura_mist") {
        return ThemePalette{
            "sakura_mist", false,
            QStringLiteral("#F3EEF5"), QStringLiteral("#E7DFEC"),
            QStringLiteral("#F9F5FB"), QStringLiteral("#F1EAF4"),
            QStringLiteral("#FCF9FD"), QStringLiteral("#ECE2F0"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#D5C8DC"), QStringLiteral("#DB2777"),
            QStringLiteral("#1E1624"), QStringLiteral("#64536E"),
            QStringLiteral("#DB2777"), QStringLiteral("#BE185D"), QStringLiteral("#DB2777"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#FCE7F3"),
            QStringLiteral("#FFFFFF"), QStringLiteral("#F7EEF5"),
            QStringLiteral("#FEE2E2"), QStringLiteral("#991B1B"), QStringLiteral("#FCA5A5")
        };
    }
    // Default: Obsidian Cyber (Dark)
    return ThemePalette{
        "obsidian_cyber", true,
        QStringLiteral("#0A0D12"), QStringLiteral("#131A24"),
        QStringLiteral("#11161F"), QStringLiteral("#151C28"),
        QStringLiteral("#161E2B"), QStringLiteral("#1B2536"),
        QStringLiteral("#0C1017"), QStringLiteral("#263142"), QStringLiteral("#38BDF8"),
        QStringLiteral("#F1F5F9"), QStringLiteral("#94A3B8"),
        QStringLiteral("#38BDF8"), QStringLiteral("#0284C7"), QStringLiteral("#38BDF8"),
        QStringLiteral("#FFFFFF"), QStringLiteral("#17293D"),
        QStringLiteral("#1A2332"), QStringLiteral("#222E42"),
        QStringLiteral("#450A0A"), QStringLiteral("#FCA5A5"), QStringLiteral("#7F1D1D")
    };
}

[[nodiscard]] QString ensureCheckmarkIconPath() {
    static const QString path = []() {
        const QString p = QDir::tempPath() + QStringLiteral("/mavira_ui_checkmark.png");
        QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter painter(&img);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawLine(QPointF(3.5, 8.2), QPointF(6.5, 11.5));
        painter.drawLine(QPointF(6.5, 11.5), QPointF(12.5, 4.5));
        painter.end();
        img.save(p, "PNG");
        return QString(p).replace(QLatin1Char('\\'), QLatin1Char('/'));
    }();
    return path;
}

} // namespace

QString MainWindow::buildStylesheet(std::string_view themeName) {
    return buildStylesheet(themeName, 100);
}

QString MainWindow::buildStylesheet(std::string_view themeName, int uiScalePercent) {
    const ThemePalette p = resolveThemePalette(themeName);
    const QString scrollThumb = p.isDark ? QStringLiteral("#334155") : QStringLiteral("#B6C2D0");
    const QString scrollThumbHover = p.isDark ? QStringLiteral("#475569") : QStringLiteral("#8FA0B5");
    const QString chkBorder = p.isDark ? QStringLiteral("#475569") : QStringLiteral("#64748B");
    const QString checkmarkPath = ensureCheckmarkIconPath();
    const double scale = static_cast<double>(
        mavira::data::config::SettingsManager::clampUiScalePercent(uiScalePercent)) / 100.0;
    const QString basePt = QString::number(9.5 * scale, 'f', 1);
    const QString badgePt = QString::number(9.5 * scale, 'f', 1);
    const QString smallPt = QString::number(8.5 * scale, 'f', 1);

    QString qss = QStringLiteral(R"qss(
        QWidget {
            font-size: %BASE_PT%pt;
        }
        QMainWindow, QWidget#MainShellRoot, QWidget#LibraryPage, QWidget#ReaderPage,
        QWidget#LibraryView, QWidget#ExplorerSidebar, QDialog,
        QWidget#ToolWorkspaceWidget, QWidget#PreferencesWorkspace,
        QWidget#ToolContentArea, QWidget#PrefsContentArea {
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 %CANVAS_START%, stop:1 %CANVAS_END%);
            color: %TEXT%;
        }
        QScrollArea#WorkspaceScrollArea,
        QWidget#WorkspaceScrollViewport,
        QWidget#WorkspaceScrollContent {
            background-color: transparent;
            color: %TEXT%;
            border: none;
        }
        QLabel {
            color: %TEXT%;
            background-color: transparent;
        }
        QCheckBox, QRadioButton {
            color: %TEXT%;
            background-color: transparent;
            spacing: 8px;
            padding: 2px 0px;
        }
        QCheckBox::indicator {
            width: 17px;
            height: 17px;
            border-radius: 5px;
            border: 2px solid %CHK_BORDER%;
            background-color: %INPUT_BG%;
        }
        QCheckBox::indicator:hover {
            border-color: %ACCENT_SOLID%;
        }
        QCheckBox::indicator:checked {
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 %ACCENT_START%, stop:1 %ACCENT_END%);
            border-color: %ACCENT_END%;
            image: url(%CHECKMARK_PATH%);
        }
        QRadioButton::indicator {
            width: 16px;
            height: 16px;
            border-radius: 9px;
            border: 2px solid %CHK_BORDER%;
            background-color: %INPUT_BG%;
        }
        QRadioButton::indicator:hover {
            border-color: %ACCENT_SOLID%;
        }
        QRadioButton::indicator:checked {
            background-color: %ACCENT_SOLID%;
            border: 4px solid %CARD_BG%;
        }
        QMenuBar#MainMenuBar {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %SURFACE%, stop:1 %SURFACE_ALT%);
            color: %TEXT%;
            border-bottom: 1px solid %BORDER%;
            padding: 3px 6px;
        }
        QMenuBar#MainMenuBar::item {
            background-color: transparent;
            padding: 5px 10px;
            border-radius: 6px;
        }
        QMenuBar#MainMenuBar::item:selected {
            background-color: %ACCENT_TINT_BG%;
            color: %ACCENT_SOLID%;
        }
        QMenu {
            background-color: %CARD_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
            border-radius: 8px;
            padding: 4px;
        }
        QMenu::item {
            padding: 6px 24px 6px 12px;
            border-radius: 6px;
        }
        QMenu::item:selected {
            background-color: %ACCENT_TINT_BG%;
            color: %ACCENT_SOLID%;
        }
        QMenu::separator {
            height: 1px;
            background: %BORDER%;
            margin: 4px 8px;
        }
        QWidget#TopCommandBar, QWidget#ReaderTopBar {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %SURFACE%, stop:1 %SURFACE_ALT%);
            border-bottom: 1px solid %BORDER%;
        }
        QWidget#ContinueReadingStrip {
            background-color: %SURFACE%;
            border-bottom: 1px solid %BORDER%;
        }
        QLabel#SectionStripHeader, QLabel#ViewHeaderLabel, QLabel#SidebarSectionTitle {
            font-weight: 700;
            font-size: %SMALL_PT%pt;
            letter-spacing: 0.6px;
            color: %MUTED_TEXT%;
            padding: 6px 12px;
            background-color: transparent;
        }
        QLabel#SidebarSectionTitle {
            padding: 2px 0px;
        }
        QWidget#SidebarSectionHeaderRow {
            background-color: transparent;
        }
        QPushButton {
            background-color: %BTN_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
            border-radius: 8px;
            padding: 6px 12px;
            font-weight: 600;
        }
        QPushButton:hover {
            background-color: %BTN_HOVER_BG%;
            border-color: %ACCENT_SOLID%;
            color: %ACCENT_SOLID%;
        }
        QPushButton:checked, QPushButton:pressed {
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %ACCENT_START%, stop:1 %ACCENT_END%);
            color: %ACCENT_TEXT%;
            border-color: %ACCENT_END%;
        }
        QPushButton#BtnSettings {
            background-color: %ACCENT_TINT_BG%;
            color: %ACCENT_SOLID%;
            border: 1px solid %ACCENT_END%;
            border-radius: 8px;
            padding: 6px 14px;
            font-weight: 700;
        }
        QPushButton#BtnSettings:hover {
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %ACCENT_START%, stop:1 %ACCENT_END%);
            color: %ACCENT_TEXT%;
        }
        QPushButton#SidebarInlinePlusBtn {
            padding: 0px;
            border-radius: 6px;
            font-weight: 700;
            background-color: %BTN_BG%;
            border: 1px solid %BORDER%;
            color: %MUTED_TEXT%;
        }
        QPushButton#SidebarInlinePlusBtn:hover {
            background-color: %ACCENT_SOLID%;
            border-color: %ACCENT_SOLID%;
            color: %ACCENT_TEXT%;
        }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit {
            background-color: %INPUT_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
            border-radius: 8px;
            padding: 5px 10px;
            selection-background-color: %ACCENT_END%;
            selection-color: %ACCENT_TEXT%;
        }
        QComboBox QAbstractItemView {
            background-color: %CARD_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
            selection-background-color: %ACCENT_TINT_BG%;
            selection-color: %ACCENT_SOLID%;
            outline: 0;
        }
        QLineEdit#FtsSearchBar {
            background-color: %INPUT_BG%;
            border: 1px solid %BORDER%;
            border-radius: 14px;
            padding: 5px 14px;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus {
            border: 1px solid %ACCENT_SOLID%;
        }
        QSlider::groove:horizontal {
            height: 6px;
            background: %BORDER%;
            border-radius: 3px;
        }
        QSlider::sub-page:horizontal {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %ACCENT_START%, stop:1 %ACCENT_END%);
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: %ACCENT_SOLID%;
            border: 2px solid %CARD_BG%;
            width: 14px;
            height: 14px;
            margin: -5px 0;
            border-radius: 8px;
        }
        QListView#VirtualizedLibraryGrid, QListView#VirtualizedLibraryGrid::viewport {
            background-color: transparent;
            color: %TEXT%;
            border: none;
            outline: 0;
        }
        QListWidget {
            background-color: %INPUT_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
            border-radius: 8px;
            outline: 0;
        }
        QListWidget::item {
            padding: 4px 8px;
            border-radius: 4px;
        }
        QListWidget::item:selected {
            background-color: %ACCENT_TINT_BG%;
            color: %ACCENT_SOLID%;
        }
        QTreeWidget#SidebarTree, QTreeWidget#SidebarTree::viewport {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %SURFACE%, stop:1 %SURFACE_ALT%);
            color: %TEXT%;
            border: none;
            outline: 0;
        }
        QTreeWidget#SidebarTree {
            border-right: 1px solid %BORDER%;
        }
        QTreeWidget#SidebarTree::item {
            padding: 4px 6px;
            border-radius: 6px;
            margin: 1px 4px;
        }
        QTreeWidget#SidebarTree::item:hover {
            background-color: %BTN_HOVER_BG%;
        }
        QTreeWidget#SidebarTree::item:selected {
            background-color: %ACCENT_TINT_BG%;
            color: %ACCENT_SOLID%;
            font-weight: 600;
        }
        QListWidget#ContinueReadingList, QListWidget#ContinueReadingList::viewport {
            background-color: transparent;
            color: %TEXT%;
            border: none;
            outline: 0;
        }
        QListWidget#ContinueReadingList::item {
            background-color: %CARD_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
            border-radius: 8px;
            padding: 4px 10px;
            margin-right: 6px;
        }
        QListWidget#ContinueReadingList::item:hover {
            border-color: %ACCENT_SOLID%;
        }
        QListWidget#ContinueReadingList::item:selected {
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %ACCENT_START%, stop:1 %ACCENT_END%);
            color: %ACCENT_TEXT%;
            border-color: %ACCENT_END%;
        }
        QTabWidget::pane {
            background-color: %CARD_BG%;
            border: 1px solid %BORDER%;
            border-radius: 8px;
        }
        QTabBar::tab {
            background-color: %SURFACE_ALT%;
            color: %MUTED_TEXT%;
            padding: 6px 14px;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background-color: %CARD_BG%;
            color: %ACCENT_SOLID%;
            font-weight: 700;
        }
        QScrollBar:vertical, QScrollBar:horizontal {
            background: transparent;
            width: 8px;
            height: 8px;
            margin: 2px;
        }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: %SCROLL_THUMB%;
            border-radius: 4px;
            min-height: 24px;
            min-width: 24px;
        }
        QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
            background: %SCROLL_THUMB_HOVER%;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            width: 0px;
            height: 0px;
        }
        QStatusBar {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %SURFACE%, stop:1 %SURFACE_ALT%);
            color: %MUTED_TEXT%;
            border-top: 1px solid %BORDER%;
        }
        QWidget#ToolSidebar, QWidget#PrefsSidebar {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %SURFACE%, stop:1 %SURFACE_ALT%);
            border-right: 1px solid %BORDER%;
        }
        QLabel#WorkspaceHeaderBadge {
            background-color: %ACCENT_TINT_BG%;
            color: %ACCENT_SOLID%;
            border: 1px solid %ACCENT_END%;
            border-radius: 8px;
            padding: 6px 14px;
            font-weight: 800;
            font-size: %BADGE_PT%pt;
            letter-spacing: 0.8px;
        }
        QLabel#SectionCardTabBadge {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %CARD_HEADER_BG%, stop:1 %CARD_BG%);
            color: %ACCENT_SOLID%;
            border: 1px solid %BORDER%;
            border-bottom: none;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
            padding: 5px 14px;
            font-weight: 700;
            font-size: %SMALL_PT%pt;
            letter-spacing: 0.6px;
        }
        QFrame#SectionCardBody {
            background-color: %CARD_BG%;
            border: 1px solid %BORDER%;
            border-top-left-radius: 0px;
            border-top-right-radius: 10px;
            border-bottom-left-radius: 10px;
            border-bottom-right-radius: 10px;
        }
        QPushButton#WorkspaceNavBtn {
            text-align: left;
            padding: 8px 14px;
            border-radius: 8px;
            background-color: transparent;
            border: 1px solid transparent;
            color: %MUTED_TEXT%;
        }
        QPushButton#WorkspaceNavBtn:hover {
            background-color: %BTN_HOVER_BG%;
            color: %TEXT%;
        }
        QPushButton#WorkspaceNavBtn:checked {
            background-color: %ACCENT_TINT_BG%;
            border: 1px solid %ACCENT_END%;
            color: %ACCENT_SOLID%;
            font-weight: 700;
        }
        QPushButton#ToolPrimaryBtn {
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %ACCENT_START%, stop:1 %ACCENT_END%);
            color: %ACCENT_TEXT%;
            border: 1px solid %ACCENT_END%;
            border-radius: 8px;
            font-weight: 700;
        }
        QPushButton#ToolPrimaryBtn:hover {
            background-color: %ACCENT_END%;
        }
        QPushButton#ToolCloseBtn {
            background-color: %BTN_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
        }
        QPushButton#ToolCancelBtn {
            background-color: %DANGER_BG%;
            color: %DANGER_TEXT%;
            border: 1px solid %DANGER_BORDER%;
        }
        QProgressBar {
            background-color: %INPUT_BG%;
            color: %TEXT%;
            border: 1px solid %BORDER%;
            border-radius: 6px;
            text-align: center;
        }
        QProgressBar::chunk {
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %ACCENT_START%, stop:1 %ACCENT_END%);
            border-radius: 5px;
        }
    )qss");

    qss.replace(QStringLiteral("%CANVAS_START%"), p.canvasStart);
    qss.replace(QStringLiteral("%CANVAS_END%"), p.canvasEnd);
    qss.replace(QStringLiteral("%SURFACE%"), p.surface);
    qss.replace(QStringLiteral("%SURFACE_ALT%"), p.surfaceAlt);
    qss.replace(QStringLiteral("%CARD_BG%"), p.cardBg);
    qss.replace(QStringLiteral("%CARD_HEADER_BG%"), p.cardHeaderBg);
    qss.replace(QStringLiteral("%INPUT_BG%"), p.inputBg);
    qss.replace(QStringLiteral("%BORDER%"), p.border);
    qss.replace(QStringLiteral("%HOVER_BORDER%"), p.hoverBorder);
    qss.replace(QStringLiteral("%TEXT%"), p.text);
    qss.replace(QStringLiteral("%MUTED_TEXT%"), p.mutedText);
    qss.replace(QStringLiteral("%ACCENT_START%"), p.accentStart);
    qss.replace(QStringLiteral("%ACCENT_END%"), p.accentEnd);
    qss.replace(QStringLiteral("%ACCENT_SOLID%"), p.accentSolid);
    qss.replace(QStringLiteral("%ACCENT_TEXT%"), p.accentText);
    qss.replace(QStringLiteral("%ACCENT_TINT_BG%"), p.accentTintBg);
    qss.replace(QStringLiteral("%BTN_BG%"), p.btnBg);
    qss.replace(QStringLiteral("%BTN_HOVER_BG%"), p.btnHoverBg);
    qss.replace(QStringLiteral("%DANGER_BG%"), p.dangerBg);
    qss.replace(QStringLiteral("%DANGER_TEXT%"), p.dangerText);
    qss.replace(QStringLiteral("%DANGER_BORDER%"), p.dangerBorder);
    qss.replace(QStringLiteral("%SCROLL_THUMB%"), scrollThumb);
    qss.replace(QStringLiteral("%SCROLL_THUMB_HOVER%"), scrollThumbHover);
    qss.replace(QStringLiteral("%CHK_BORDER%"), chkBorder);
    qss.replace(QStringLiteral("%CHECKMARK_PATH%"), checkmarkPath);
    qss.replace(QStringLiteral("%BASE_PT%"), basePt);
    qss.replace(QStringLiteral("%BADGE_PT%"), badgePt);
    qss.replace(QStringLiteral("%SMALL_PT%"), smallPt);
    return qss;
}

void MainWindow::applyTheme(const std::string& themeName, bool persist) {
    const ThemePalette p = resolveThemePalette(themeName);
    currentTheme_ = p.id;

    // Sync QPalette so native checkboxes, radio buttons, and tooltips match the active theme
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(p.canvasStart));
    pal.setColor(QPalette::WindowText, QColor(p.text));
    pal.setColor(QPalette::Base, QColor(p.inputBg));
    pal.setColor(QPalette::AlternateBase, QColor(p.cardBg));
    pal.setColor(QPalette::ToolTipBase, QColor(p.cardBg));
    pal.setColor(QPalette::ToolTipText, QColor(p.text));
    pal.setColor(QPalette::Text, QColor(p.text));
    pal.setColor(QPalette::Button, QColor(p.btnBg));
    pal.setColor(QPalette::ButtonText, QColor(p.text));
    pal.setColor(QPalette::Highlight, QColor(p.accentEnd));
    pal.setColor(QPalette::HighlightedText, QColor(p.accentText));
    if (qApp != nullptr) {
        qApp->setPalette(pal);
    }
    setPalette(pal);

    setStyleSheet(buildStylesheet(currentTheme_, settings_.ui_scale_percent));

    if (libraryView_ != nullptr) {
        libraryView_->setThemePalette(
            p.isDark,
            QColor(p.cardBg),
            QColor(p.canvasStart),
            QColor(p.accentSolid),
            QColor(p.border),
            QColor(p.hoverBorder),
            QColor(p.text),
            QColor(p.mutedText));
    }
    if (readerViewport_ != nullptr) {
        readerViewport_->setDarkTheme(p.isDark);
    }

    if (persist && settingsManager_ != nullptr) {
        settings_.theme = currentTheme_;
        (void)settingsManager_->save(settings_);
    }

    emit themeChanged(QString::fromStdString(currentTheme_));
}

void MainWindow::toggleTheme() {
    const auto& presets = mavira::data::config::SettingsManager::kThemePresets;
    std::size_t nextIdx = 0;
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (presets[i] == currentTheme_) {
            nextIdx = (i + 1) % presets.size();
            break;
        }
    }
    applyTheme(std::string(presets[nextIdx]));
}

void MainWindow::applyUiScale(int percent) {
    const int clamped = mavira::data::config::SettingsManager::clampUiScalePercent(percent);
    const double basePt = (baseAppFont_.pointSizeF() > 0.0) ? baseAppFont_.pointSizeF() : 9.5;
    QFont scaledFont = baseAppFont_;
    scaledFont.setPointSizeF(basePt * (static_cast<double>(clamped) / 100.0));
    if (qApp != nullptr) {
        qApp->setFont(scaledFont);
    }
    setFont(scaledFont);
}

void MainWindow::retranslateUi() {
    if (menuBar() != nullptr) {
        menuBar()->clear();
        setupMenuBar();
    }

    if (btnAddFolder_ != nullptr) btnAddFolder_->setText(LocalizationManager::tr("btn.add_folder"));
    if (btnOpenFile_ != nullptr) btnOpenFile_->setText(LocalizationManager::tr("btn.open_file"));
    if (btnRandomUnread_ != nullptr) btnRandomUnread_->setText(LocalizationManager::tr("btn.random_unread"));
    if (searchEdit_ != nullptr) searchEdit_->setPlaceholderText(LocalizationManager::tr("search.placeholder"));
    if (cmbSortBy_ != nullptr && cmbSortBy_->count() == 4) {
        cmbSortBy_->setItemText(0, LocalizationManager::tr("sort.title_az"));
        cmbSortBy_->setItemText(1, LocalizationManager::tr("sort.recently_added"));
        cmbSortBy_->setItemText(2, LocalizationManager::tr("sort.last_read"));
        cmbSortBy_->setItemText(3, LocalizationManager::tr("sort.file_size"));
    }
    if (cmbStatusFilter_ != nullptr && cmbStatusFilter_->count() == 4) {
        cmbStatusFilter_->setItemText(0, LocalizationManager::tr("filter.all"));
        cmbStatusFilter_->setItemText(1, LocalizationManager::tr("filter.unread"));
        cmbStatusFilter_->setItemText(2, LocalizationManager::tr("filter.in_progress"));
        cmbStatusFilter_->setItemText(3, LocalizationManager::tr("filter.finished"));
    }
    if (btnLibraryView_ != nullptr) btnLibraryView_->setText(LocalizationManager::tr("btn.library"));
    if (btnReaderView_ != nullptr) btnReaderView_->setText(LocalizationManager::tr("btn.reader"));
    if (btnSettings_ != nullptr) btnSettings_->setText(LocalizationManager::tr("btn.settings"));
    if (btnBackToLibrary_ != nullptr) btnBackToLibrary_->setText(LocalizationManager::tr("btn.back_to_library"));
    if (continueReadingTitleLabel_ != nullptr) {
        continueReadingTitleLabel_->setText(LocalizationManager::tr("strip.continue_reading"));
    }
    if (readerController_ != nullptr && !readerController_->isOpen() && readerTitleLabel_ != nullptr) {
        readerTitleLabel_->setText(LocalizationManager::tr("reader.no_volume"));
    }
    if (statusLabel_ != nullptr && (statusLabel_->text().isEmpty() || statusLabel_->text() == QStringLiteral("Ready"))) {
        statusLabel_->setText(LocalizationManager::tr("status.ready"));
    }
    if (sidebar_ != nullptr) sidebar_->retranslateUi();
    if (readerViewport_ != nullptr) readerViewport_->retranslateUi();
    if (prefsWorkspace_ != nullptr) prefsWorkspace_->retranslateUi();
    if (toolWorkspace_ != nullptr) toolWorkspace_->retranslateUi();
}

void MainWindow::applySettings(
    const mavira::data::config::AppSettings& newSettings,
    bool persist) {
    const int prevCacheMode = settings_.cover_cache_mode;
    const bool prevShowHidden = settings_.show_hidden_items;
    settings_ = newSettings;

    LocalizationManager::setLanguage(settings_.language);
    applyUiScale(settings_.ui_scale_percent);
    applyTheme(settings_.theme.empty() ? "obsidian_cyber" : settings_.theme, /*persist=*/false);

    if (libraryManager_ != nullptr) {
        libraryManager_->setSmartRename(settings_.smart_rename);
    }
    if (libraryView_ != nullptr) {
        libraryView_->setDisplayOptions(settings_.circular_progress_bar, settings_.smart_rename);
        libraryView_->setCardDensity(settings_.card_density);
        libraryView_->setExternalAppPath(
            mavira::data::fs::FileManager::utf8ToWide(settings_.external_app_path));
    }
    retranslateUi();
    updateScreenKeepAlive();
    if (readerController_ != nullptr) {
        readerController_->applySettings(settings_);
    }
    if (readerViewport_ != nullptr) {
        readerViewport_->applySettings(settings_);
    }

    if (cacheManager_ != nullptr) {
        const std::size_t ramBytes =
            static_cast<std::size_t>(std::max(32, settings_.ram_cache_limit_mb)) * 1024ULL * 1024ULL;
        cacheManager_->pageCache().setMemoryLimitBytes(ramBytes);

        if (cacheManager_->coverStorage() == nullptr ||
            prevCacheMode != settings_.cover_cache_mode ||
            (settings_.cover_cache_mode == 2 &&
             dynamic_cast<mavira::core::cache::PackFileCoverProvider*>(cacheManager_->coverStorage()) == nullptr) ||
            (settings_.cover_cache_mode != 2 &&
             dynamic_cast<mavira::core::cache::RamOnlyCoverProvider*>(cacheManager_->coverStorage()) == nullptr)) {
            if (settings_.cover_cache_mode == 2 && db_ != nullptr) {
                const auto packPath = (libraryManager_ != nullptr)
                    ? libraryManager_->portablePaths().coversPackPath
                    : mavira::data::fs::FileManager::resolvePortablePaths().coversPackPath;
                cacheManager_->setCoverStorage(
                    std::make_unique<mavira::core::cache::PackFileCoverProvider>(packPath, db_));
            } else {
                cacheManager_->setCoverStorage(
                    std::make_unique<mavira::core::cache::RamOnlyCoverProvider>());
            }
            if (libraryView_ != nullptr) {
                libraryView_->clearCoverCache();
            }
        }
    }

    if (persist && settingsManager_ != nullptr) {
        (void)settingsManager_->save(settings_);
    }

    if (prevShowHidden != settings_.show_hidden_items) {
        refreshLibraryView();
    }
}

void MainWindow::openToolWorkspace(
    const QString& toolId,
    const std::vector<std::filesystem::path>& initialPaths) {
    (void)ensureToolWorkspace();
    if (toolWorkspace_ != nullptr && toolWorkspace_->isToolRunning() &&
        toolWorkspace_->currentToolId() != toolId) {
        toolWorkspace_->openTool(toolId, initialPaths);
        switchToView(ActiveViewMode::ToolWorkspace);
        return;
    }
    if (activeViewMode_ == ActiveViewMode::Library || activeViewMode_ == ActiveViewMode::Reader) {
        previousViewMode_ = activeViewMode_;
    }
    if (toolWorkspace_ != nullptr) {
        toolWorkspace_->openTool(toolId, initialPaths);
    }
    switchToView(ActiveViewMode::ToolWorkspace);
}

void MainWindow::openPreferencesWorkspace() {
    (void)ensurePreferencesWorkspace();
    if (activeViewMode_ == ActiveViewMode::Library || activeViewMode_ == ActiveViewMode::Reader) {
        previousViewMode_ = activeViewMode_;
    }
    if (prefsWorkspace_ != nullptr) {
        const auto paths = (libraryManager_ != nullptr)
            ? libraryManager_->portablePaths()
            : mavira::data::fs::FileManager::resolvePortablePaths();
        prefsWorkspace_->setPortablePaths(paths);
        prefsWorkspace_->loadFromSettings(settings_);
    }
    switchToView(ActiveViewMode::Preferences);
}

std::vector<std::filesystem::path> MainWindow::selectedLibraryPaths() const {
    std::vector<std::filesystem::path> paths;
    if (libraryView_ != nullptr) {
        for (const auto& rec : libraryView_->selectedComics()) {
            if (!rec.filePath.empty()) {
                paths.push_back(rec.filePath);
            }
        }
    }
    return paths;
}

void MainWindow::openMergeComicsDialog(const std::vector<std::filesystem::path>& initialFiles) {
    openToolWorkspace(
        QStringLiteral("merge_comics"),
        initialFiles.empty() ? selectedLibraryPaths() : initialFiles);
}

void MainWindow::openConvertComicDialog() {
    openToolWorkspace(QStringLiteral("convert_comics"), selectedLibraryPaths());
}

void MainWindow::openBatchConvertImagesDialog() {
    openToolWorkspace(QStringLiteral("convert_images"), selectedLibraryPaths());
}

void MainWindow::openCreateComicDialog() {
    openToolWorkspace(QStringLiteral("create_comic"), selectedLibraryPaths());
}

void MainWindow::openExtractPagesDialog() {
    openToolWorkspace(QStringLiteral("extract_pages"), selectedLibraryPaths());
}

void MainWindow::openNativeOcrDialog() {
    openToolWorkspace(QStringLiteral("extract_ocr"), selectedLibraryPaths());
}

void MainWindow::openShortcutsDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    dlg.resize(460, 340);
    auto* layout = new QVBoxLayout(&dlg);
    auto* text = new QPlainTextEdit(&dlg);
    text->setReadOnly(true);
    text->setPlainText(QStringLiteral(
        "General & Library\n"
        "  Ctrl+O          Open Comic / Document File\n"
        "  Ctrl+Shift+O    Add Watched Folder\n"
        "  Ctrl+A          Select All Comics\n"
        "  Delete          Remove Selected from Library\n"
        "  Shift+Delete    Delete Selected to Recycle Bin\n"
        "  Ctrl+,          Preferences / Settings\n\n"
        "Reader\n"
        "  Left / Right    Previous / Next Page\n"
        "  Space / PgDn    Next Page\n"
        "  PgUp            Previous Page\n"
        "  Home / End      First / Last Page\n"
        "  + / -           Zoom In / Zoom Out\n"
        "  0               Reset Zoom\n"
        "  F11             Toggle Fullscreen\n"
        "  Esc             Back to Library\n"));
    layout->addWidget(text, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::openSettingsDialog() {
    openPreferencesWorkspace();
}

void MainWindow::openAboutDialog() {
    auto readNoticeText = [](const wchar_t* fileName, const QString& fallbackText) -> QString {
        const std::filesystem::path exeDir =
            mavira::data::fs::FileManager::getExecutableDirectory();
        const std::vector<std::filesystem::path> candidates = {
            exeDir / fileName,
            exeDir.parent_path() / fileName,
            exeDir.parent_path().parent_path() / fileName
        };
        for (const auto& candidate : candidates) {
            QFile file(QString::fromStdWString(candidate.native()));
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return QString::fromUtf8(file.readAll());
            }
        }
        return fallbackText;
    };

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("About MAVIRA"));
    dlg.setObjectName(QStringLiteral("AboutDialog"));
    dlg.resize(720, 520);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(12);

    auto* logoLabel = new QLabel(&dlg);
    logoLabel->setFixedSize(64, 64);
    logoLabel->setAlignment(Qt::AlignCenter);
    QPixmap logoPixmap;
    if (const QString svgPath = resolveAssetPath(L"assets/logo.svg"); !svgPath.isEmpty()) {
        logoPixmap.load(svgPath);
    }
    if (logoPixmap.isNull()) {
        if (const QString icoPath = resolveAssetPath(L"assets/app.ico"); !icoPath.isEmpty()) {
            logoPixmap = QIcon(icoPath).pixmap(64, 64);
        }
    }
    if (logoPixmap.isNull() && !windowIcon().isNull()) {
        logoPixmap = windowIcon().pixmap(64, 64);
    }
    if (!logoPixmap.isNull()) {
        logoLabel->setPixmap(
            logoPixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    auto* headerLabel = new QLabel(
        QStringLiteral("<b>MAVIRA - Native C++20 Batch Comic Converter, Extractor, Merger &amp; Offline OCR Suite</b> (v1.1.0)<br/>"
                       "Portable Windows x64 Archive Utility &amp; Built-In Quick Preview Reader<br/>"
                       "Licensed under the MIT License."),
        &dlg);
    headerLabel->setWordWrap(true);

    headerRow->addWidget(logoLabel);
    headerRow->addWidget(headerLabel, 1);
    layout->addLayout(headerRow);

    auto* tabs = new QTabWidget(&dlg);

    auto* licenseEdit = new QPlainTextEdit(tabs);
    licenseEdit->setReadOnly(true);
    licenseEdit->setPlainText(readNoticeText(
        L"LICENSE",
        QStringLiteral("MIT License\n\nCopyright (c) 2026 MAVIRA Contributors")));
    tabs->addTab(licenseEdit, QStringLiteral("License (MIT)"));

    auto* noticesEdit = new QPlainTextEdit(tabs);
    noticesEdit->setReadOnly(true);
    noticesEdit->setPlainText(readNoticeText(
        L"THIRD_PARTY_NOTICES.txt",
        QStringLiteral("Third-Party Open-Source Notices:\n"
                       "Qt 6 (LGPLv3, dynamically linked), PDFium (BSD-3-Clause), "
                       "libzip (BSD-3-Clause), libarchive (BSD-2-Clause), "
                       "libjpeg-turbo (IJG/BSD), libpng (libpng/zlib), "
                       "libwebp (BSD-3-Clause), SQLite3 (Public Domain), spdlog (MIT).")));
    tabs->addTab(noticesEdit, QStringLiteral("Third-Party Notices"));

    layout->addWidget(tabs, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    dlg.exec();
}

void MainWindow::switchToView(ActiveViewMode mode) {
    if (mode == ActiveViewMode::Reader) {
        (void)ensureReaderViewport();
    } else if (mode == ActiveViewMode::ToolWorkspace) {
        (void)ensureToolWorkspace();
    } else if (mode == ActiveViewMode::Preferences) {
        (void)ensurePreferencesWorkspace();
    }
    activeViewMode_ = mode;
    if (centralStack_ != nullptr) {
        centralStack_->setCurrentIndex(static_cast<int>(mode));
    }
    if (sidebar_ != nullptr) {
        const bool showSidebar = (mode == ActiveViewMode::Library);
        sidebar_->setVisible(showSidebar);
    }
    if (btnLibraryView_ != nullptr) {
        btnLibraryView_->setChecked(mode == ActiveViewMode::Library);
    }
    if (btnReaderView_ != nullptr) {
        btnReaderView_->setChecked(mode == ActiveViewMode::Reader);
    }
    updateScreenKeepAlive();
    emit viewModeChanged(mode);
}

void MainWindow::refreshContinueReadingStrip() {
    if (continueReadingList_ == nullptr || db_ == nullptr || !db_->isOpen()) {
        return;
    }

    continueReadingList_->clear();
    continueReadingComics_.clear();

    auto crRes = db_->getContinueReading(20, settings_.show_hidden_items);
    if (!crRes || crRes.value().empty()) {
        continueReadingContainer_->setVisible(false);
        return;
    }

    continueReadingComics_ = std::move(crRes.value());
    continueReadingContainer_->setVisible(true);

    for (std::size_t i = 0; i < continueReadingComics_.size(); ++i) {
        const auto& comic = continueReadingComics_[i];
        const QString title = displayTitleForComic(comic);
        const QString label = QStringLiteral("%1\nPage %2 / %3")
                                  .arg(title)
                                  .arg(comic.currentPage + 1)
                                  .arg(std::max(1u, comic.pageCount));
        auto* item = new QListWidgetItem(label, continueReadingList_);
        item->setData(Qt::UserRole, static_cast<int>(i));
        item->setToolTip(QString::fromStdWString(comic.filePath.native()));
    }
}

void MainWindow::refreshLibraryView() {
    refreshContinueReadingStrip();

    if (searchEdit_ != nullptr && !searchEdit_->text().trimmed().isEmpty()) {
        executeSearch(searchEdit_->text());
        return;
    }

    if (sidebar_ != nullptr) {
        onSidebarNavigationSelected(sidebar_->currentSelection());
    }
}

void MainWindow::executeSearch(const QString& queryText) {
    if (db_ == nullptr || !db_->isOpen()) {
        return;
    }

    const QString trimmed = queryText.trimmed();
    if (trimmed.isEmpty()) {
        if (sidebar_ != nullptr) {
            onSidebarNavigationSelected(sidebar_->currentSelection());
        }
        return;
    }

    auto res = db_->searchComics(trimmed.toStdString(), 500, settings_.show_hidden_items);
    if (!res) {
        return;
    }
    populateLibraryTable(
        res.value(),
        QStringLiteral("SEARCH RESULTS: \"%1\" (%2)").arg(trimmed).arg(res.value().size()));
}

void MainWindow::onSearchTextChanged(const QString& text) {
    executeSearch(text);
}

std::vector<mavira::data::db::ComicRecord> MainWindow::evaluateSmartViewFilter(
    const QString& queryJson) {
    std::vector<mavira::data::db::ComicRecord> all;
    if (db_ == nullptr || !db_->isOpen()) {
        return all;
    }

    auto allRes = db_->getAllComics(0, settings_.show_hidden_items);
    if (!allRes) {
        return all;
    }
    all = std::move(allRes.value());

    const QJsonDocument doc = QJsonDocument::fromJson(queryJson.toUtf8());
    if (!doc.isObject()) {
        return all;
    }
    const QJsonObject obj = doc.object();

    if (obj.contains(QStringLiteral("preset"))) {
        const QString preset = obj.value(QStringLiteral("preset")).toString();
        std::vector<mavira::data::db::ComicRecord> filtered;
        for (const auto& c : all) {
            if (preset == QStringLiteral("unread") && !c.isRead && c.currentPage == 0) {
                filtered.push_back(c);
            } else if (preset == QStringLiteral("in_progress") && !c.isRead && c.currentPage > 0) {
                filtered.push_back(c);
            } else if (preset == QStringLiteral("completed") && c.isRead) {
                filtered.push_back(c);
            } else if (preset == QStringLiteral("offline_pinned") && c.isOfflinePinned) {
                filtered.push_back(c);
            } else if (preset == QStringLiteral("large_volumes") &&
                       c.fileSizeBytes >= 100ULL * 1024ULL * 1024ULL) {
                filtered.push_back(c);
            }
        }
        return filtered;
    }

    const QJsonArray rules = obj.value(QStringLiteral("rules")).toArray();
    if (rules.isEmpty()) {
        return all;
    }

    std::vector<mavira::data::db::ComicRecord> filtered;
    for (const auto& c : all) {
        bool matchesAll = true;
        for (const auto& ruleVal : rules) {
            const QJsonObject r = ruleVal.toObject();
            const QString field = r.value(QStringLiteral("field")).toString();
            const QString op = r.value(QStringLiteral("operator")).toString();
            const QString val = r.value(QStringLiteral("value")).toString();

            QString candidate;
            if (field == QStringLiteral("title")) {
                candidate = QString::fromStdString(c.title);
            } else if (field == QStringLiteral("series")) {
                candidate = QString::fromStdString(c.series);
            } else if (field == QStringLiteral("author")) {
                candidate = QString::fromStdString(c.author);
            } else if (field == QStringLiteral("tags")) {
                candidate = QString::fromStdString(c.tags);
            } else if (field == QStringLiteral("is_read")) {
                candidate = c.isRead ? QStringLiteral("true") : QStringLiteral("false");
            }

            if (!matchesStringRule(candidate, op, val)) {
                matchesAll = false;
                break;
            }
        }
        if (matchesAll) {
            filtered.push_back(c);
        }
    }
    return filtered;
}

void MainWindow::onSidebarNavigationSelected(const SidebarFilterSelection& selection) {
    if (db_ == nullptr || !db_->isOpen()) {
        return;
    }

    std::vector<mavira::data::db::ComicRecord> records;
    QString headerTitle = selection.label.toUpper();

    isAllComicsView_ = (selection.kind == SidebarNodeKind::AllComics);
    switch (selection.kind) {
        case SidebarNodeKind::AllComics: {
            const std::string statusKey = (cmbStatusFilter_ != nullptr)
                ? cmbStatusFilter_->currentData().toString().toStdString()
                : "all";
            const std::string sortKey = (cmbSortBy_ != nullptr)
                ? cmbSortBy_->currentData().toString().toStdString()
                : "title_az";
            if (auto res = db_->queryComicsFiltered(statusKey, sortKey, settings_.show_hidden_items, 0); res) {
                displayedComics_ = std::move(res.value());
            } else {
                displayedComics_.clear();
            }
            currentViewTitle_ = QStringLiteral("ALL COMICS (%1)").arg(displayedComics_.size());
            if (viewHeaderLabel_ != nullptr) {
                viewHeaderLabel_->setText(currentViewTitle_);
            }
            if (libraryView_ != nullptr) {
                libraryView_->setComics(displayedComics_);
            }
            return;
        }
        case SidebarNodeKind::Favorites:
        case SidebarNodeKind::ShelfItem: {
            if (auto res = db_->getComicsInShelf(selection.recordId, settings_.show_hidden_items); res) {
                records = std::move(res.value());
            }
            headerTitle = QStringLiteral("SHELF: %1 (%2)")
                              .arg(selection.label.toUpper())
                              .arg(records.size());
            break;
        }
        case SidebarNodeKind::WatchedFolder:
        case SidebarNodeKind::ChildFolder:
        case SidebarNodeKind::CloudSource: {
            const std::filesystem::path folderPath(selection.payload.toStdWString());
            if (auto res = db_->getComicsInFolder(folderPath, settings_.show_hidden_items); res) {
                records = std::move(res.value());
            }
            headerTitle = QStringLiteral("FOLDER: %1 (%2)")
                              .arg(selection.payload)
                              .arg(records.size());
            break;
        }
        case SidebarNodeKind::SeriesBucket: {
            if (auto allRes = db_->getAllComics(0, settings_.show_hidden_items); allRes) {
                const QString bucket = selection.payload.toUpper();
                for (auto& c : allRes.value()) {
                    if (!c.series.empty() && ExplorerSidebar::seriesBucketForName(c.series) == bucket) {
                        records.push_back(std::move(c));
                    }
                }
            }
            headerTitle = QStringLiteral("SERIES [%1] (%2)")
                              .arg(selection.payload)
                              .arg(records.size());
            break;
        }
        case SidebarNodeKind::SeriesItem: {
            if (auto res = db_->getComicsBySeries(selection.payload.toStdString(), settings_.show_hidden_items); res) {
                records = std::move(res.value());
            }
            headerTitle = QStringLiteral("SERIES: %1 (%2)")
                              .arg(selection.payload)
                              .arg(records.size());
            break;
        }
        case SidebarNodeKind::SmartViewPreset:
        case SidebarNodeKind::SmartViewCustom: {
            records = evaluateSmartViewFilter(selection.payload);
            headerTitle = QStringLiteral("SMARTVIEW: %1 (%2)")
                              .arg(selection.label.toUpper())
                              .arg(records.size());
            break;
        }
        case SidebarNodeKind::SectionHeader:
            return;
    }

    populateLibraryTable(records, headerTitle);
}

void MainWindow::populateLibraryTable(
    const std::vector<mavira::data::db::ComicRecord>& comics,
    const QString& viewTitle) {
    isAllComicsView_ = false;
    unfilteredComics_ = comics;
    currentViewTitle_ = viewTitle;
    applySortAndStatusFilter();
}

void MainWindow::applySortAndStatusFilter() {
    const QString statusKey = (cmbStatusFilter_ != nullptr)
        ? cmbStatusFilter_->currentData().toString()
        : QStringLiteral("all");
    const QString sortKey = (cmbSortBy_ != nullptr)
        ? cmbSortBy_->currentData().toString()
        : QStringLiteral("title_az");

    // Fast path for All Comics (30,000+ titles): execute indexed SQLite WHERE + ORDER BY directly.
    if (isAllComicsView_ && db_ != nullptr && db_->isOpen()) {
        if (auto res = db_->queryComicsFiltered(
                statusKey.toStdString(),
                sortKey.toStdString(),
                settings_.show_hidden_items,
                0); res) {
            displayedComics_ = std::move(res.value());
        } else {
            displayedComics_.clear();
        }
        currentViewTitle_ = QStringLiteral("ALL COMICS (%1)").arg(displayedComics_.size());
        if (viewHeaderLabel_ != nullptr) {
            viewHeaderLabel_->setText(currentViewTitle_);
        }
        if (libraryView_ != nullptr) {
            libraryView_->setComics(displayedComics_);
        }
        return;
    }

    displayedComics_.clear();
    displayedComics_.reserve(unfilteredComics_.size());
    for (const auto& c : unfilteredComics_) {
        if (statusKey == QStringLiteral("unread") && (c.isRead || c.currentPage > 0)) {
            continue;
        }
        if (statusKey == QStringLiteral("in_progress") && (c.isRead || c.currentPage == 0)) {
            continue;
        }
        if (statusKey == QStringLiteral("finished") && !c.isRead) {
            continue;
        }
        displayedComics_.push_back(c);
    }

    if (sortKey == QStringLiteral("added_desc")) {
        std::sort(displayedComics_.begin(), displayedComics_.end(), [](const auto& a, const auto& b) {
            return a.addedUnix > b.addedUnix;
        });
    } else if (sortKey == QStringLiteral("last_read_desc")) {
        std::sort(displayedComics_.begin(), displayedComics_.end(), [](const auto& a, const auto& b) {
            return a.lastReadUnix > b.lastReadUnix;
        });
    } else if (sortKey == QStringLiteral("size_desc")) {
        std::sort(displayedComics_.begin(), displayedComics_.end(), [](const auto& a, const auto& b) {
            return a.fileSizeBytes > b.fileSizeBytes;
        });
    } else {
        std::sort(displayedComics_.begin(), displayedComics_.end(), [](const auto& a, const auto& b) {
            return mavira::data::fs::FileManager::naturalCompare(a.title, b.title) < 0;
        });
    }

    if (viewHeaderLabel_ != nullptr) {
        viewHeaderLabel_->setText(
            statusKey == QStringLiteral("all")
                ? currentViewTitle_
                : QStringLiteral("%1 - %2").arg(currentViewTitle_).arg(displayedComics_.size()));
    }
    if (libraryView_ != nullptr) {
        libraryView_->setComics(displayedComics_);
    }
}

void MainWindow::triggerRandomUnread() {
    if (db_ == nullptr || !db_->isOpen()) {
        return;
    }
    auto rndRes = db_->getRandomUnread(settings_.show_hidden_items);
    if (!rndRes || !rndRes.value().has_value()) {
        if (statusLabel_ != nullptr) {
            statusLabel_->setText(QStringLiteral("No unread comics found in library."));
        }
        return;
    }
    openComicInReader(*rndRes.value());
}

void MainWindow::openComicInReader(const mavira::data::db::ComicRecord& comic) {
    const std::filesystem::path readablePath = (libraryManager_ != nullptr)
        ? libraryManager_->resolveReadablePath(comic)
        : comic.filePath;
    const QString qPath = QString::fromStdWString(readablePath.native());
    const QString title = displayTitleForComic(comic);

    if (readerTitleLabel_ != nullptr) {
        readerTitleLabel_->setText(
            QStringLiteral("%1 - Page %2 / %3")
                .arg(title)
                .arg(comic.currentPage + 1)
                .arg(std::max(1u, comic.pageCount)));
    }

    if (readerController_ != nullptr) {
        auto openRes = readerController_->openComic(
            readablePath,
            comic.id,
            static_cast<std::size_t>(comic.currentPage));
        if (!openRes.isOk()) {
            const bool volumeOnline = mavira::data::fs::FileManager::isVolumeOnline(readablePath);
            const bool fileMissing = !mavira::data::fs::FileManager::isPathOnline(readablePath);
            if (comic.id > 0 && libraryManager_ != nullptr && volumeOnline && fileMissing) {
                (void)libraryManager_->removeComicFromLibrary(comic.id, /*deleteFromDisk=*/false);
                if (statusLabel_ != nullptr) {
                    statusLabel_->setText(
                        QStringLiteral("Comic file no longer exists on disk - removed stale entry."));
                }
            } else if (statusLabel_ != nullptr) {
                statusLabel_->setText(QString::fromStdString(openRes.error().message));
            }
            return;
        }
    }

    switchToView(ActiveViewMode::Reader);
    if (readerViewport_ != nullptr) {
        readerViewport_->setFocus();
    }
    refreshTaskbarJumpList();
    emit comicOpenedInReader(comic.id, qPath);
}

void MainWindow::openComicPath(const std::filesystem::path& filePath) {
    if (filePath.empty()) {
        return;
    }

    if (db_ != nullptr && db_->isOpen()) {
        if (auto existing = db_->getComicByPath(filePath); existing && existing.value().has_value()) {
            openComicInReader(*existing.value());
            return;
        }
    }

    mavira::data::db::ComicRecord tempRec;
    tempRec.filePath = filePath;
    tempRec.title = mavira::data::fs::FileManager::wideToUtf8(filePath.stem().native());
    tempRec.format = mavira::data::fs::FileManager::detectFormatFromExtension(filePath);
    openComicInReader(tempRec);
}

void MainWindow::onContinueReadingItemActivated(QListWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const int idx = item->data(Qt::UserRole).toInt();
    if (idx >= 0 && static_cast<std::size_t>(idx) < continueReadingComics_.size()) {
        openComicInReader(continueReadingComics_[static_cast<std::size_t>(idx)]);
    }
}

void MainWindow::addFolderPathAsync(const std::filesystem::path& folderPath) {
    if (folderPath.empty() || libraryManager_ == nullptr) {
        return;
    }
    // Never scans on the UI thread: addWatchedFolder(..., true) delegates scan to WorkerPool.
    auto res = libraryManager_->addWatchedFolder(folderPath, /*triggerScan=*/true);
    if (res && sidebar_ != nullptr) {
        sidebar_->refreshNavigation();
    }
}

void MainWindow::promptAddWatchedFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Comic / Manga Library Folder"),
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        addFolderPathAsync(std::filesystem::path(dir.toStdWString()));
    }
}

void MainWindow::promptOpenComicFile() {
    const QString file = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open Comic / Manga / PDF Volume"),
        QString(),
        QStringLiteral("Comic & Document Files (*.cbz *.zip *.cbr *.rar *.cb7 *.7z *.cbt *.tar *.epub *.pdf)"));
    if (file.isEmpty()) {
        return;
    }

    const std::filesystem::path filePath(file.toStdWString());
    openComicPath(filePath);
}

void MainWindow::onScanProgress(int processed, int total, const QString& currentFolder) {
    if (scanProgressBar_ != nullptr) {
        scanProgressBar_->setVisible(true);
        scanProgressBar_->setRange(0, std::max(1, total));
        scanProgressBar_->setValue(processed);
    }
    if (statusLabel_ != nullptr) {
        statusLabel_->setText(
            QStringLiteral("Indexing %1 (%2 / %3)...")
                .arg(currentFolder)
                .arg(processed)
                .arg(total));
    }
}

void MainWindow::onScanFinished(int indexedCount, int prunedCount, bool cancelled) {
    if (scanProgressBar_ != nullptr) {
        scanProgressBar_->setVisible(false);
    }
    if (statusLabel_ != nullptr) {
        statusLabel_->setText(
            cancelled
                ? QStringLiteral("Scan cancelled.")
                : QStringLiteral("Library ready - %1 indexed, %2 pruned.")
                      .arg(indexedCount)
                      .arg(prunedCount));
    }
    if (sidebar_ != nullptr) {
        sidebar_->refreshNavigation();
    }
    refreshLibraryView();
}

} // namespace mavira::ui
