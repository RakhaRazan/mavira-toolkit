#include "ui/ToolsDialogs.hpp"

#include "core/archive/ComicMerger.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "core/imaging/NativeOcrEngine.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <filesystem>
#include <vector>

namespace mavira::ui {

// =============================================================================
// 1. ConvertComicDialog
// =============================================================================
ConvertComicDialog::ConvertComicDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Convert Comic Book"));
    resize(560, 260);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    auto* inRow = new QHBoxLayout();
    inputEdit_ = new QLineEdit(this);
    auto* btnIn = new QPushButton(QStringLiteral("Browse..."), this);
    inRow->addWidget(inputEdit_, 1);
    inRow->addWidget(btnIn);
    form->addRow(QStringLiteral("Input Comic:"), inRow);

    auto* outRow = new QHBoxLayout();
    outputEdit_ = new QLineEdit(this);
    auto* btnOut = new QPushButton(QStringLiteral("Save As..."), this);
    outRow->addWidget(outputEdit_, 1);
    outRow->addWidget(btnOut);
    form->addRow(QStringLiteral("Output File:"), outRow);

    formatCombo_ = new QComboBox(this);
    formatCombo_->addItem(QStringLiteral("CBZ (Zero-Recompression Archive)"), static_cast<int>(mavira::core::ComicFormat::CBZ));
    formatCombo_->addItem(QStringLiteral("PDF (Multi-Page Document)"), static_cast<int>(mavira::core::ComicFormat::PDF));
    form->addRow(QStringLiteral("Target Format:"), formatCombo_);

    qualitySpin_ = new QSpinBox(this);
    qualitySpin_->setRange(10, 100);
    qualitySpin_->setValue(92);
    form->addRow(QStringLiteral("PDF JPEG Quality:"), qualitySpin_);

    layout->addLayout(form);

    progressBar_ = new QProgressBar(this);
    progressBar_->setVisible(false);
    statusLabel_ = new QLabel(QStringLiteral("Select a comic book (.cbr, .cb7, .pdf, .epub, .cbz) to convert."), this);
    layout->addWidget(progressBar_);
    layout->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    runBtn_ = new QPushButton(QStringLiteral("Convert"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    btnRow->addWidget(runBtn_);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    connect(btnIn, &QPushButton::clicked, this, &ConvertComicDialog::browseInput);
    connect(btnOut, &QPushButton::clicked, this, &ConvertComicDialog::browseOutput);
    connect(runBtn_, &QPushButton::clicked, this, &ConvertComicDialog::runConversion);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void ConvertComicDialog::browseInput() {
    const QString file = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Input Comic / Document"),
        QString(),
        QStringLiteral("Comics & Documents (*.cbr *.rar *.cb7 *.7z *.pdf *.epub *.cbz *.zip *.cbt *.tar)"));
    if (!file.isEmpty()) {
        inputEdit_->setText(file);
        if (outputEdit_->text().trimmed().isEmpty()) {
            const std::filesystem::path p(file.toStdWString());
            const auto fmt = static_cast<mavira::core::ComicFormat>(formatCombo_->currentData().toInt());
            const std::wstring ext = (fmt == mavira::core::ComicFormat::PDF) ? L".pdf" : L".cbz";
            std::filesystem::path suggested = p.parent_path() / (p.stem().native() + L"_converted" + ext);
            outputEdit_->setText(QString::fromStdWString(suggested.native()));
        }
    }
}

void ConvertComicDialog::browseOutput() {
    const auto fmt = static_cast<mavira::core::ComicFormat>(formatCombo_->currentData().toInt());
    const QString filter = (fmt == mavira::core::ComicFormat::PDF)
        ? QStringLiteral("PDF Document (*.pdf)")
        : QStringLiteral("Comic Book Archive (*.cbz)");
    const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Save Converted Comic"), outputEdit_->text(), filter);
    if (!file.isEmpty()) {
        outputEdit_->setText(file);
    }
}

void ConvertComicDialog::runConversion() {
    const std::filesystem::path inPath(inputEdit_->text().trimmed().toStdWString());
    const std::filesystem::path outPath(outputEdit_->text().trimmed().toStdWString());
    const auto targetFmt = static_cast<mavira::core::ComicFormat>(formatCombo_->currentData().toInt());

    progressBar_->setVisible(true);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    runBtn_->setEnabled(false);

    auto res = mavira::core::archive::ComicMerger::convertComicBook(
        inPath,
        outPath,
        targetFmt,
        qualitySpin_->value(),
        true,
        [this](const mavira::core::archive::MergeProgress& p) {
            if (p.totalPagesEstimated > 0) {
                progressBar_->setRange(0, static_cast<int>(p.totalPagesEstimated));
                progressBar_->setValue(static_cast<int>(p.totalPagesProcessed));
            }
            QApplication::processEvents();
        });

    runBtn_->setEnabled(true);
    if (res.isOk()) {
        statusLabel_->setText(QStringLiteral("Converted %1 pages successfully.").arg(res.value().totalPages));
    } else {
        statusLabel_->setText(QStringLiteral("Conversion failed: %1").arg(QString::fromStdString(res.error().message)));
    }
}

// =============================================================================
// 2. BatchConvertImagesDialog
// =============================================================================
BatchConvertImagesDialog::BatchConvertImagesDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Batch Convert Images"));
    resize(560, 280);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    auto* inRow = new QHBoxLayout();
    inputsEdit_ = new QLineEdit(this);
    inputsEdit_->setReadOnly(true);
    auto* btnIn = new QPushButton(QStringLiteral("Select Images..."), this);
    inRow->addWidget(inputsEdit_, 1);
    inRow->addWidget(btnIn);
    form->addRow(QStringLiteral("Input Images:"), inRow);

    auto* outRow = new QHBoxLayout();
    outputDirEdit_ = new QLineEdit(this);
    auto* btnOut = new QPushButton(QStringLiteral("Browse..."), this);
    outRow->addWidget(outputDirEdit_, 1);
    outRow->addWidget(btnOut);
    form->addRow(QStringLiteral("Output Folder:"), outRow);

    formatCombo_ = new QComboBox(this);
    formatCombo_->addItem(QStringLiteral("JPEG (.jpg)"), static_cast<int>(mavira::core::imaging::ImageFormat::JPEG));
    formatCombo_->addItem(QStringLiteral("PNG (.png)"), static_cast<int>(mavira::core::imaging::ImageFormat::PNG));
    formatCombo_->addItem(QStringLiteral("WebP (.webp)"), static_cast<int>(mavira::core::imaging::ImageFormat::WEBP));
    form->addRow(QStringLiteral("Target Format:"), formatCombo_);

    maxWidthSpin_ = new QSpinBox(this);
    maxWidthSpin_->setRange(0, 16384);
    maxWidthSpin_->setValue(0);
    maxWidthSpin_->setSpecialValueText(QStringLiteral("Original Width"));
    form->addRow(QStringLiteral("Max Width (px):"), maxWidthSpin_);

    qualitySpin_ = new QSpinBox(this);
    qualitySpin_->setRange(10, 100);
    qualitySpin_->setValue(85);
    form->addRow(QStringLiteral("Quality (JPEG/WebP):"), qualitySpin_);

    layout->addLayout(form);

    progressBar_ = new QProgressBar(this);
    progressBar_->setVisible(false);
    statusLabel_ = new QLabel(QStringLiteral("Select image files to batch convert."), this);
    layout->addWidget(progressBar_);
    layout->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    runBtn_ = new QPushButton(QStringLiteral("Convert All"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    btnRow->addWidget(runBtn_);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    connect(btnIn, &QPushButton::clicked, this, &BatchConvertImagesDialog::browseInputs);
    connect(btnOut, &QPushButton::clicked, this, &BatchConvertImagesDialog::browseOutputDir);
    connect(runBtn_, &QPushButton::clicked, this, &BatchConvertImagesDialog::runConversion);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void BatchConvertImagesDialog::browseInputs() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Select Images to Convert"),
        QString(),
        QStringLiteral("Images (*.jpg *.jpeg *.png *.webp *.bmp *.gif)"));
    if (!files.isEmpty()) {
        selectedFiles_ = files;
        inputsEdit_->setText(QStringLiteral("%1 image(s) selected").arg(files.size()));
    }
}

void BatchConvertImagesDialog::browseOutputDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Select Output Folder"));
    if (!dir.isEmpty()) {
        outputDirEdit_->setText(dir);
    }
}

void BatchConvertImagesDialog::runConversion() {
    std::vector<std::filesystem::path> inputs;
    inputs.reserve(static_cast<std::size_t>(selectedFiles_.size()));
    for (const QString& f : selectedFiles_) {
        inputs.emplace_back(f.toStdWString());
    }
    const std::filesystem::path outDir(outputDirEdit_->text().trimmed().toStdWString());
    const auto fmt = static_cast<mavira::core::imaging::ImageFormat>(formatCombo_->currentData().toInt());

    progressBar_->setVisible(true);
    progressBar_->setRange(0, static_cast<int>(inputs.size()));
    progressBar_->setValue(0);
    runBtn_->setEnabled(false);

    auto res = mavira::core::archive::ComicMerger::batchConvertImages(
        inputs,
        outDir,
        fmt,
        static_cast<std::uint32_t>(maxWidthSpin_->value()),
        qualitySpin_->value(),
        [this](const mavira::core::archive::MergeProgress& p) {
            progressBar_->setValue(static_cast<int>(p.totalPagesProcessed));
            QApplication::processEvents();
        });

    runBtn_->setEnabled(true);
    if (res.isOk()) {
        statusLabel_->setText(QStringLiteral("Converted %1 image(s) successfully.").arg(res.value()));
    } else {
        statusLabel_->setText(QStringLiteral("Batch conversion failed: %1").arg(QString::fromStdString(res.error().message)));
    }
}

// =============================================================================
// 3. CreateComicDialog
// =============================================================================
CreateComicDialog::CreateComicDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Create Comic Book from Folder"));
    resize(560, 230);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    auto* inRow = new QHBoxLayout();
    folderEdit_ = new QLineEdit(this);
    auto* btnIn = new QPushButton(QStringLiteral("Browse..."), this);
    inRow->addWidget(folderEdit_, 1);
    inRow->addWidget(btnIn);
    form->addRow(QStringLiteral("Image Folder:"), inRow);

    auto* outRow = new QHBoxLayout();
    outputEdit_ = new QLineEdit(this);
    auto* btnOut = new QPushButton(QStringLiteral("Save As..."), this);
    outRow->addWidget(outputEdit_, 1);
    outRow->addWidget(btnOut);
    form->addRow(QStringLiteral("Output File:"), outRow);

    formatCombo_ = new QComboBox(this);
    formatCombo_->addItem(QStringLiteral("CBZ (Zero-Recompression)"), static_cast<int>(mavira::core::ComicFormat::CBZ));
    formatCombo_->addItem(QStringLiteral("PDF (Multi-Page Document)"), static_cast<int>(mavira::core::ComicFormat::PDF));
    form->addRow(QStringLiteral("Output Format:"), formatCombo_);

    layout->addLayout(form);

    progressBar_ = new QProgressBar(this);
    progressBar_->setVisible(false);
    statusLabel_ = new QLabel(QStringLiteral("Select a folder of images to package into a comic book."), this);
    layout->addWidget(progressBar_);
    layout->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    runBtn_ = new QPushButton(QStringLiteral("Create"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    btnRow->addWidget(runBtn_);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    connect(btnIn, &QPushButton::clicked, this, &CreateComicDialog::browseFolder);
    connect(btnOut, &QPushButton::clicked, this, &CreateComicDialog::browseOutput);
    connect(runBtn_, &QPushButton::clicked, this, &CreateComicDialog::runCreate);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void CreateComicDialog::browseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Select Image Folder"));
    if (!dir.isEmpty()) {
        folderEdit_->setText(dir);
        if (outputEdit_->text().trimmed().isEmpty()) {
            const std::filesystem::path p(dir.toStdWString());
            const auto fmt = static_cast<mavira::core::ComicFormat>(formatCombo_->currentData().toInt());
            const std::wstring ext = (fmt == mavira::core::ComicFormat::PDF) ? L".pdf" : L".cbz";
            outputEdit_->setText(QString::fromStdWString((p.parent_path() / (p.filename().native() + ext)).native()));
        }
    }
}

void CreateComicDialog::browseOutput() {
    const auto fmt = static_cast<mavira::core::ComicFormat>(formatCombo_->currentData().toInt());
    const QString filter = (fmt == mavira::core::ComicFormat::PDF)
        ? QStringLiteral("PDF Document (*.pdf)")
        : QStringLiteral("Comic Book Archive (*.cbz)");
    const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Save Comic Book"), outputEdit_->text(), filter);
    if (!file.isEmpty()) {
        outputEdit_->setText(file);
    }
}

void CreateComicDialog::runCreate() {
    const std::filesystem::path inDir(folderEdit_->text().trimmed().toStdWString());
    const std::filesystem::path outPath(outputEdit_->text().trimmed().toStdWString());
    const auto fmt = static_cast<mavira::core::ComicFormat>(formatCombo_->currentData().toInt());

    progressBar_->setVisible(true);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    runBtn_->setEnabled(false);

    auto cb = [this](const mavira::core::archive::MergeProgress& p) {
        if (p.totalPagesEstimated > 0) {
            progressBar_->setRange(0, static_cast<int>(p.totalPagesEstimated));
            progressBar_->setValue(static_cast<int>(p.totalPagesProcessed));
        }
        QApplication::processEvents();
    };

    bool ok = false;
    std::size_t pages = 0;
    QString errMsg;
    if (fmt == mavira::core::ComicFormat::CBZ) {
        auto res = mavira::core::archive::ComicMerger::createCbzFromFolder(inDir, outPath, cb);
        ok = res.isOk();
        if (ok) pages = res.value(); else errMsg = QString::fromStdString(res.error().message);
    } else {
        auto res = mavira::core::archive::ComicMerger::convertComicBook(inDir, outPath, fmt, 92, true, cb);
        ok = res.isOk();
        if (ok) pages = res.value().totalPages; else errMsg = QString::fromStdString(res.error().message);
    }

    runBtn_->setEnabled(true);
    statusLabel_->setText(
        ok ? QStringLiteral("Created comic book with %1 pages.").arg(pages)
           : QStringLiteral("Creation failed: %1").arg(errMsg));
}

// =============================================================================
// 4. ExtractPagesDialog
// =============================================================================
ExtractPagesDialog::ExtractPagesDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Extract Pages from Comic Book"));
    resize(560, 210);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    auto* inRow = new QHBoxLayout();
    archiveEdit_ = new QLineEdit(this);
    auto* btnIn = new QPushButton(QStringLiteral("Browse..."), this);
    inRow->addWidget(archiveEdit_, 1);
    inRow->addWidget(btnIn);
    form->addRow(QStringLiteral("Comic Archive:"), inRow);

    auto* outRow = new QHBoxLayout();
    outputDirEdit_ = new QLineEdit(this);
    auto* btnOut = new QPushButton(QStringLiteral("Browse..."), this);
    outRow->addWidget(outputDirEdit_, 1);
    outRow->addWidget(btnOut);
    form->addRow(QStringLiteral("Output Folder:"), outRow);

    layout->addLayout(form);

    progressBar_ = new QProgressBar(this);
    progressBar_->setVisible(false);
    statusLabel_ = new QLabel(QStringLiteral("Select a comic or PDF to extract all pages into a folder."), this);
    layout->addWidget(progressBar_);
    layout->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    runBtn_ = new QPushButton(QStringLiteral("Extract"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    btnRow->addWidget(runBtn_);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    connect(btnIn, &QPushButton::clicked, this, &ExtractPagesDialog::browseArchive);
    connect(btnOut, &QPushButton::clicked, this, &ExtractPagesDialog::browseOutputFolder);
    connect(runBtn_, &QPushButton::clicked, this, &ExtractPagesDialog::runExtract);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void ExtractPagesDialog::browseArchive() {
    const QString file = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Comic / Document"),
        QString(),
        QStringLiteral("Comics & Documents (*.cbz *.zip *.cbr *.rar *.cb7 *.7z *.cbt *.tar *.epub *.pdf)"));
    if (!file.isEmpty()) {
        archiveEdit_->setText(file);
    }
}

void ExtractPagesDialog::browseOutputFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Select Output Folder"));
    if (!dir.isEmpty()) {
        outputDirEdit_->setText(dir);
    }
}

void ExtractPagesDialog::runExtract() {
    const std::filesystem::path src(archiveEdit_->text().trimmed().toStdWString());
    const std::filesystem::path dst(outputDirEdit_->text().trimmed().toStdWString());

    progressBar_->setVisible(true);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    runBtn_->setEnabled(false);

    auto res = mavira::core::archive::ComicMerger::extractPagesToFolder(
        src,
        dst,
        [this](const mavira::core::archive::MergeProgress& p) {
            if (p.totalPagesEstimated > 0) {
                progressBar_->setRange(0, static_cast<int>(p.totalPagesEstimated));
                progressBar_->setValue(static_cast<int>(p.totalPagesProcessed));
            }
            QApplication::processEvents();
        });

    runBtn_->setEnabled(true);
    statusLabel_->setText(
        res.isOk()
            ? QStringLiteral("Extracted %1 pages successfully.").arg(res.value())
            : QStringLiteral("Extraction failed: %1").arg(QString::fromStdString(res.error().message)));
}

// =============================================================================
// 5. NativeOcrDialog
// =============================================================================
NativeOcrDialog::NativeOcrDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Extract Text from Image (Native Windows OCR)"));
    resize(620, 440);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    auto* inRow = new QHBoxLayout();
    imageEdit_ = new QLineEdit(this);
    auto* btnIn = new QPushButton(QStringLiteral("Browse..."), this);
    inRow->addWidget(imageEdit_, 1);
    inRow->addWidget(btnIn);
    form->addRow(QStringLiteral("Image File:"), inRow);

    languageCombo_ = new QComboBox(this);
    languageCombo_->addItem(QStringLiteral("Auto (User Profile Languages)"), QString());
    for (const auto& lang : mavira::core::imaging::NativeOcrEngine::availableLanguages()) {
        languageCombo_->addItem(
            QStringLiteral("%1 (%2)")
                .arg(QString::fromStdString(lang.displayName))
                .arg(QString::fromStdString(lang.languageTag)),
            QString::fromStdString(lang.languageTag));
    }
    form->addRow(QStringLiteral("OCR Language:"), languageCombo_);
    layout->addLayout(form);

    textOutput_ = new QPlainTextEdit(this);
    textOutput_->setReadOnly(true);
    textOutput_->setPlaceholderText(QStringLiteral("Extracted text will appear here (100% offline Windows 10/11 OCR)..."));
    layout->addWidget(textOutput_, 1);

    statusLabel_ = new QLabel(QStringLiteral("Ready"), this);
    layout->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    ocrBtn_ = new QPushButton(QStringLiteral("Extract Text"), this);
    copyBtn_ = new QPushButton(QStringLiteral("Copy Text"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    btnRow->addWidget(ocrBtn_);
    btnRow->addWidget(copyBtn_);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    connect(btnIn, &QPushButton::clicked, this, &NativeOcrDialog::browseImage);
    connect(ocrBtn_, &QPushButton::clicked, this, &NativeOcrDialog::runOcr);
    connect(copyBtn_, &QPushButton::clicked, this, &NativeOcrDialog::copyText);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void NativeOcrDialog::browseImage() {
    const QString file = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Image for OCR"),
        QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
    if (!file.isEmpty()) {
        imageEdit_->setText(file);
        runOcr();
    }
}

void NativeOcrDialog::runOcr() {
    const std::filesystem::path imgPath(imageEdit_->text().trimmed().toStdWString());
    if (imgPath.empty()) {
        statusLabel_->setText(QStringLiteral("Please select an image file first."));
        return;
    }

    const std::string langTag = languageCombo_->currentData().toString().toStdString();
    ocrBtn_->setEnabled(false);
    statusLabel_->setText(QStringLiteral("Running offline Windows OCR..."));
    QApplication::processEvents();

    auto res = mavira::core::imaging::NativeOcrEngine::recognizeImageFile(imgPath, langTag);
    ocrBtn_->setEnabled(true);

    if (res.isOk()) {
        textOutput_->setPlainText(QString::fromStdString(res.value().text));
        statusLabel_->setText(
            QStringLiteral("Recognized %1 line(s) [%2].")
                .arg(res.value().lines.size())
                .arg(QString::fromStdString(res.value().languageTag)));
    } else {
        statusLabel_->setText(QStringLiteral("OCR failed: %1").arg(QString::fromStdString(res.error().message)));
    }
}

void NativeOcrDialog::copyText() {
    if (auto* clipboard = QApplication::clipboard()) {
        clipboard->setText(textOutput_->toPlainText());
        statusLabel_->setText(QStringLiteral("Copied extracted text to clipboard."));
    }
}

} // namespace mavira::ui
