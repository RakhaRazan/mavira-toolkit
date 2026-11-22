#include "ui/MergeComicsDialog.hpp"

#include "data/fs/FileManager.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace mavira::ui {

namespace {

using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::Result;
using mavira::core::archive::ComicMerger;
using mavira::core::archive::MergeOptions;
using mavira::core::archive::MergeProgress;
using mavira::core::archive::MergeResult;

} // namespace

MergeComicsDialog::MergeComicsDialog(
    mavira::data::db::DatabaseManager* db,
    const std::vector<std::filesystem::path>& initialFiles,
    QWidget* parent)
    : QDialog(parent),
      db_(db) {
    setWindowTitle(QStringLiteral("Batch Comic Merger"));
    setModal(true);
    resize(660, 500);

    setupUi();
    if (!initialFiles.empty()) {
        addInputFiles(initialFiles);
    }
    updateButtonStates();
}

MergeComicsDialog::~MergeComicsDialog() {
    mergeStopSource_.request_stop();
    if (mergeWorkerThread_.joinable()) {
        mergeWorkerThread_.join();
    }
}

void MergeComicsDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* lblIntro = new QLabel(
        QStringLiteral("Add and order source volumes (.cbr, .cbz, .cb7, .cbt, .pdf). "
                       "Pages stream 1-at-a-time (< 40 MB RAM, zero temp extraction)."),
        this);
    lblIntro->setWordWrap(true);
    root->addWidget(lblIntro);

    // Input list + reorder toolbar
    auto* listRow = new QHBoxLayout();
    listRow->setSpacing(8);

    fileList_ = new QListWidget(this);
    fileList_->setObjectName(QStringLiteral("MergeInputList"));
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileList_->setDragDropMode(QAbstractItemView::InternalMove);
    fileList_->setDefaultDropAction(Qt::MoveAction);
    fileList_->setAlternatingRowColors(true);

    auto* listBtnCol = new QVBoxLayout();
    listBtnCol->setSpacing(6);

    btnAddFiles_ = new QPushButton(QStringLiteral("Add Files..."), this);
    btnRemove_ = new QPushButton(QStringLiteral("Remove"), this);
    btnMoveUp_ = new QPushButton(QStringLiteral("Move Up"), this);
    btnMoveDown_ = new QPushButton(QStringLiteral("Move Down"), this);
    btnClear_ = new QPushButton(QStringLiteral("Clear"), this);

    listBtnCol->addWidget(btnAddFiles_);
    listBtnCol->addWidget(btnRemove_);
    listBtnCol->addSpacing(8);
    listBtnCol->addWidget(btnMoveUp_);
    listBtnCol->addWidget(btnMoveDown_);
    listBtnCol->addSpacing(8);
    listBtnCol->addWidget(btnClear_);
    listBtnCol->addStretch(1);

    listRow->addWidget(fileList_, 1);
    listRow->addLayout(listBtnCol);
    root->addLayout(listRow, 1);

    // Output configuration form
    auto* form = new QFormLayout();
    form->setSpacing(8);

    cmbFormat_ = new QComboBox(this);
    cmbFormat_->setObjectName(QStringLiteral("MergeFormatCombo"));
    cmbFormat_->addItem(
        QStringLiteral(".cbz - Zero-Recompression Store (Default, 100% Original Quality)"),
        static_cast<int>(ComicFormat::CBZ));
    cmbFormat_->addItem(
        QStringLiteral(".cb7 - 7-Zip LZMA2 Archive (Open-Source High Compression)"),
        static_cast<int>(ComicFormat::CB7));
    cmbFormat_->addItem(
        QStringLiteral(".pdf - Multi-Page PDF 1.4 Document"),
        static_cast<int>(ComicFormat::PDF));

    auto* outPathRow = new QHBoxLayout();
    outPathRow->setSpacing(6);
    editOutputPath_ = new QLineEdit(this);
    editOutputPath_->setObjectName(QStringLiteral("MergeOutputPathEdit"));
    editOutputPath_->setPlaceholderText(QStringLiteral("Select destination merged file..."));
    btnBrowseOutput_ = new QPushButton(QStringLiteral("Browse..."), this);
    outPathRow->addWidget(editOutputPath_, 1);
    outPathRow->addWidget(btnBrowseOutput_);

    chkSaveBookmarks_ = new QCheckBox(
        QStringLiteral("Save volume start pages as chapter bookmarks"), this);
    chkSaveBookmarks_->setChecked(true);

    form->addRow(QStringLiteral("Output Format:"), cmbFormat_);
    form->addRow(QStringLiteral("Output File:"), outPathRow);
    form->addRow(QString(), chkSaveBookmarks_);
    root->addLayout(form);

    // Live progress section
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName(QStringLiteral("MergeProgressBar"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);

    lblStatus_ = new QLabel(QStringLiteral("Ready"), this);
    lblStatus_->setObjectName(QStringLiteral("MergeStatusLabel"));
    lblStatus_->setWordWrap(true);

    root->addWidget(progressBar_);
    root->addWidget(lblStatus_);

    // Action row
    auto* actionRow = new QHBoxLayout();
    actionRow->addStretch(1);
    btnStart_ = new QPushButton(QStringLiteral("Merge"), this);
    btnStart_->setObjectName(QStringLiteral("BtnStartMerge"));
    btnStart_->setDefault(true);
    btnCancel_ = new QPushButton(QStringLiteral("Close"), this);
    btnCancel_->setObjectName(QStringLiteral("BtnCancelMerge"));
    actionRow->addWidget(btnStart_);
    actionRow->addWidget(btnCancel_);
    root->addLayout(actionRow);

    // Signal wiring
    connect(btnAddFiles_, &QPushButton::clicked, this, &MergeComicsDialog::promptAddFiles);
    connect(btnRemove_, &QPushButton::clicked, this, &MergeComicsDialog::removeSelectedFiles);
    connect(btnMoveUp_, &QPushButton::clicked, this, &MergeComicsDialog::moveSelectedUp);
    connect(btnMoveDown_, &QPushButton::clicked, this, &MergeComicsDialog::moveSelectedDown);
    connect(btnClear_, &QPushButton::clicked, this, &MergeComicsDialog::clearFiles);
    connect(btnBrowseOutput_, &QPushButton::clicked, this, &MergeComicsDialog::promptOutputPath);
    connect(btnStart_, &QPushButton::clicked, this, &MergeComicsDialog::startMerge);
    connect(btnCancel_, &QPushButton::clicked, this, [this]() {
        if (isMerging()) {
            cancelMerge();
        } else {
            reject();
        }
    });
    connect(cmbFormat_, &QComboBox::currentIndexChanged, this, &MergeComicsDialog::onFormatChanged);
    connect(editOutputPath_, &QLineEdit::textChanged, this, &MergeComicsDialog::updateButtonStates);
    connect(fileList_, &QListWidget::itemSelectionChanged, this, &MergeComicsDialog::updateButtonStates);
}

QString MergeComicsDialog::extensionForFormat(ComicFormat fmt) {
    switch (fmt) {
        case ComicFormat::PDF: return QStringLiteral(".pdf");
        case ComicFormat::CB7: return QStringLiteral(".cb7");
        case ComicFormat::CBZ:
        default:               return QStringLiteral(".cbz");
    }
}

void MergeComicsDialog::addInputFiles(const std::vector<std::filesystem::path>& paths) {
    for (const auto& p : paths) {
        if (p.empty()) {
            continue;
        }
        const QString fullPath = QString::fromStdWString(p.native());
        const QString fileName = QString::fromStdWString(p.filename().native());
        auto* item = new QListWidgetItem(fileName.isEmpty() ? fullPath : fileName, fileList_);
        item->setData(Qt::UserRole, fullPath);
        item->setToolTip(fullPath);
    }

    if (editOutputPath_->text().trimmed().isEmpty() && !paths.empty()) {
        const auto parentDir = paths.front().parent_path();
        const std::wstring stem = paths.front().stem().native();
        const QString ext = extensionForFormat(selectedOutputFormat());
        const std::filesystem::path suggested =
            parentDir / (stem + L"_merged" + ext.toStdWString());
        editOutputPath_->setText(QString::fromStdWString(suggested.native()));
    }

    updateButtonStates();
}

void MergeComicsDialog::setOutputPath(const std::filesystem::path& path) {
    editOutputPath_->setText(QString::fromStdWString(path.native()));
    updateButtonStates();
}

void MergeComicsDialog::setOutputFormat(ComicFormat format) {
    for (int i = 0; i < cmbFormat_->count(); ++i) {
        if (static_cast<ComicFormat>(cmbFormat_->itemData(i).toInt()) == format) {
            cmbFormat_->setCurrentIndex(i);
            break;
        }
    }
}

std::vector<std::filesystem::path> MergeComicsDialog::orderedInputFiles() const {
    std::vector<std::filesystem::path> result;
    result.reserve(static_cast<std::size_t>(fileList_->count()));
    for (int i = 0; i < fileList_->count(); ++i) {
        const QListWidgetItem* item = fileList_->item(i);
        if (item == nullptr) {
            continue;
        }
        const QString fullPath = item->data(Qt::UserRole).toString();
        if (!fullPath.isEmpty()) {
            result.emplace_back(fullPath.toStdWString());
        }
    }
    return result;
}

ComicFormat MergeComicsDialog::selectedOutputFormat() const {
    return static_cast<ComicFormat>(cmbFormat_->currentData().toInt());
}

std::filesystem::path MergeComicsDialog::outputPath() const {
    const QString text = editOutputPath_->text().trimmed();
    return text.isEmpty() ? std::filesystem::path{} : std::filesystem::path(text.toStdWString());
}

void MergeComicsDialog::promptAddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Select Comic / Manga / PDF Volumes to Merge"),
        QString(),
        QStringLiteral("Comic & Document Files (*.cbr *.rar *.cbz *.zip *.cb7 *.7z *.cbt *.tar *.pdf);;All Files (*.*)"));
    if (files.isEmpty()) {
        return;
    }

    std::vector<std::filesystem::path> paths;
    paths.reserve(static_cast<std::size_t>(files.size()));
    for (const QString& f : files) {
        paths.emplace_back(f.toStdWString());
    }
    addInputFiles(paths);
}

void MergeComicsDialog::removeSelectedFiles() {
    const QList<QListWidgetItem*> selected = fileList_->selectedItems();
    for (QListWidgetItem* item : selected) {
        delete fileList_->takeItem(fileList_->row(item));
    }
    updateButtonStates();
}

void MergeComicsDialog::moveSelectedUp() {
    const int row = fileList_->currentRow();
    if (row <= 0) {
        return;
    }
    QListWidgetItem* item = fileList_->takeItem(row);
    fileList_->insertItem(row - 1, item);
    fileList_->setCurrentRow(row - 1);
    updateButtonStates();
}

void MergeComicsDialog::moveSelectedDown() {
    const int row = fileList_->currentRow();
    if (row < 0 || row >= fileList_->count() - 1) {
        return;
    }
    QListWidgetItem* item = fileList_->takeItem(row);
    fileList_->insertItem(row + 1, item);
    fileList_->setCurrentRow(row + 1);
    updateButtonStates();
}

void MergeComicsDialog::clearFiles() {
    fileList_->clear();
    updateButtonStates();
}

void MergeComicsDialog::promptOutputPath() {
    const ComicFormat fmt = selectedOutputFormat();
    QString filter;
    switch (fmt) {
        case ComicFormat::PDF:
            filter = QStringLiteral("PDF Document (*.pdf)");
            break;
        case ComicFormat::CB7:
            filter = QStringLiteral("Comic Book 7-Zip (*.cb7)");
            break;
        case ComicFormat::CBZ:
        default:
            filter = QStringLiteral("Comic Book ZIP (*.cbz)");
            break;
    }

    const QString chosen = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Merged Output File"),
        editOutputPath_->text(),
        filter);
    if (!chosen.isEmpty()) {
        editOutputPath_->setText(chosen);
    }
}

void MergeComicsDialog::onFormatChanged(int /*index*/) {
    const ComicFormat fmt = selectedOutputFormat();

    // Automatically swap the output extension if the user already has a destination path.
    const QString current = editOutputPath_->text().trimmed();
    if (!current.isEmpty()) {
        std::filesystem::path p(current.toStdWString());
        p.replace_extension(extensionForFormat(fmt).toStdWString());
        editOutputPath_->setText(QString::fromStdWString(p.native()));
    }
    updateButtonStates();
}

void MergeComicsDialog::setControlsEnabledDuringMerge(bool enabled) {
    fileList_->setEnabled(enabled);
    btnAddFiles_->setEnabled(enabled);
    btnRemove_->setEnabled(enabled);
    btnMoveUp_->setEnabled(enabled);
    btnMoveDown_->setEnabled(enabled);
    btnClear_->setEnabled(enabled);
    cmbFormat_->setEnabled(enabled);
    editOutputPath_->setEnabled(enabled);
    btnBrowseOutput_->setEnabled(enabled);
    chkSaveBookmarks_->setEnabled(enabled);
    btnStart_->setEnabled(enabled);
    btnCancel_->setText(enabled ? QStringLiteral("Close") : QStringLiteral("Cancel"));
}

void MergeComicsDialog::updateButtonStates() {
    if (isMerging()) {
        return;
    }
    const int count = fileList_->count();
    const int row = fileList_->currentRow();
    const bool hasSelection = !fileList_->selectedItems().isEmpty();

    btnRemove_->setEnabled(hasSelection);
    btnMoveUp_->setEnabled(row > 0);
    btnMoveDown_->setEnabled(row >= 0 && row < count - 1);
    btnClear_->setEnabled(count > 0);

    const bool canStart = (count > 0) && !editOutputPath_->text().trimmed().isEmpty();
    btnStart_->setEnabled(canStart);
}

void MergeComicsDialog::startMerge() {
    if (isMerging()) {
        return;
    }

    const auto inputs = orderedInputFiles();
    const auto outPath = outputPath();
    if (inputs.empty() || outPath.empty()) {
        lblStatus_->setText(QStringLiteral("Please select input files and an output path."));
        return;
    }

    if (mergeWorkerThread_.joinable()) {
        mergeWorkerThread_.join();
    }

    MergeOptions options;
    options.inputPaths = inputs;
    options.outputPath = outPath;
    options.outputFormat = selectedOutputFormat();
    options.saveChapterBookmarks = chkSaveBookmarks_->isChecked();
    options.overwriteExisting = true;
    options.database = db_;

    lastResult_.reset();
    mergeStopSource_ = std::stop_source{};
    const std::stop_token token = mergeStopSource_.get_token();

    merging_.store(true, std::memory_order_release);
    setControlsEnabledDuringMerge(false);
    progressBar_->setRange(0, 0);
    lblStatus_->setText(QStringLiteral("Scanning %1 volumes...").arg(inputs.size()));

    mergeWorkerThread_ = std::jthread([this, options = std::move(options), token]() {
        auto progressCb = [this](const MergeProgress& prog) {
            const int processed = static_cast<int>(prog.totalPagesProcessed);
            const int total = static_cast<int>(std::max<std::size_t>(1, prog.totalPagesEstimated));
            const QString volName = QString::fromStdString(prog.currentVolumeName);
            const int volNum = static_cast<int>(prog.currentVolumeIndex + 1);
            const int volTotal = static_cast<int>(prog.totalVolumes);
            const int pageNum = static_cast<int>(prog.currentPageInVolume);
            const int pageTotal = static_cast<int>(prog.pagesInCurrentVolume);

            QMetaObject::invokeMethod(
                this,
                [this, processed, total, volName, volNum, volTotal, pageNum, pageTotal]() {
                    progressBar_->setRange(0, total);
                    progressBar_->setValue(processed);
                    lblStatus_->setText(
                        QStringLiteral("Volume %1/%2 (%3) - Page %4/%5 (%6/%7 total)")
                            .arg(volNum)
                            .arg(volTotal)
                            .arg(volName)
                            .arg(pageNum)
                            .arg(pageTotal)
                            .arg(processed)
                            .arg(total));
                },
                Qt::QueuedConnection);
        };

        auto res = ComicMerger::merge(options, progressCb, token);

        QMetaObject::invokeMethod(
            this,
            [this, res = std::move(res)]() mutable {
                merging_.store(false, std::memory_order_release);
                setControlsEnabledDuringMerge(true);
                updateButtonStates();

                if (res.isOk()) {
                    lastResult_ = std::move(res).value();
                    const int totalPages = static_cast<int>(lastResult_->totalPages);
                    progressBar_->setRange(0, std::max(1, totalPages));
                    progressBar_->setValue(totalPages);
                    lblStatus_->setText(
                        QStringLiteral("Merge complete: %1 pages across %2 volumes (%3 bookmarks).")
                            .arg(lastResult_->totalPages)
                            .arg(lastResult_->totalVolumes)
                            .arg(lastResult_->chapterBookmarks.size()));
                    emit mergeCompleted(
                        QString::fromStdWString(lastResult_->outputPath.native()),
                        totalPages,
                        static_cast<qint64>(lastResult_->savedComicId));
                } else if (res.errorCode() == ErrorCode::Cancelled) {
                    progressBar_->setRange(0, 100);
                    progressBar_->setValue(0);
                    lblStatus_->setText(QStringLiteral("Merge cancelled."));
                    emit mergeCancelled();
                } else {
                    const QString errMsg = QString::fromStdString(res.error().message);
                    progressBar_->setRange(0, 100);
                    progressBar_->setValue(0);
                    lblStatus_->setText(QStringLiteral("Merge failed: %1").arg(errMsg));
                    emit mergeFailed(errMsg);
                }
            },
            Qt::QueuedConnection);
    });
}

void MergeComicsDialog::cancelMerge() {
    if (isMerging()) {
        mergeStopSource_.request_stop();
        lblStatus_->setText(QStringLiteral("Cancelling merge..."));
        btnCancel_->setEnabled(false);
    }
}

void MergeComicsDialog::reject() {
    if (isMerging()) {
        cancelMerge();
        if (mergeWorkerThread_.joinable()) {
            mergeWorkerThread_.join();
        }
    }
    QDialog::reject();
}

} // namespace mavira::ui
