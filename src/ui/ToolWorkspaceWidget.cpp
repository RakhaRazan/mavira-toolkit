#include "ui/ToolWorkspaceWidget.hpp"

#include "core/imaging/NativeOcrEngine.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/LocalizationManager.hpp"
#include "ui/SectionCardWidget.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace mavira::ui {

namespace {

[[nodiscard]] QWidget* wrapInScrollArea(QWidget* content, QWidget* parent) {
    content->setObjectName(QStringLiteral("WorkspaceScrollContent"));
    content->setAutoFillBackground(false);
    auto* scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("WorkspaceScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setObjectName(QStringLiteral("WorkspaceScrollViewport"));
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(content);
    return scroll;
}

} // namespace

ToolWorkspaceWidget::ToolWorkspaceWidget(
    mavira::data::config::SettingsManager* settingsManager,
    QWidget* parent)
    : QWidget(parent),
      settingsManager_(settingsManager) {
    setObjectName(QStringLiteral("ToolWorkspaceWidget"));
    setupUi();
    configureForTool(QStringLiteral("convert_comics"));
}

ToolWorkspaceWidget::~ToolWorkspaceWidget() {
    stopSource_.request_stop();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void ToolWorkspaceWidget::setupUi() {
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // =========================================================================
    // Left Sidebar (240px)
    // =========================================================================
    auto* sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("ToolSidebar"));
    sidebar->setMinimumWidth(240);
    sidebar->setMaximumWidth(360);
    sidebar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(14, 16, 14, 16);
    sideLayout->setSpacing(10);

    btnCloseTool_ = new QPushButton(QStringLiteral("CLOSE TOOL"), sidebar);
    btnCloseTool_->setObjectName(QStringLiteral("ToolCloseBtn"));
    btnCloseTool_->setMinimumHeight(36);

    btnExecute_ = new QPushButton(QStringLiteral("CONVERT"), sidebar);
    btnExecute_->setObjectName(QStringLiteral("ToolPrimaryBtn"));
    btnExecute_->setMinimumHeight(40);

    progressBar_ = new QProgressBar(sidebar);
    progressBar_->setObjectName(QStringLiteral("ToolProgressBar"));
    progressBar_->setVisible(false);
    progressBar_->setTextVisible(true);
    progressBar_->setStyleSheet(QStringLiteral(
        "QProgressBar { "
        "  border: 1px solid #2A3241; "
        "  border-radius: 6px; "
        "  background-color: #141820; "
        "  color: #E2E8F0; "
        "  text-align: center; "
        "  min-height: 18px; "
        "} "
        "QProgressBar::chunk { "
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3B82F6, stop:1 #06B6D4); "
        "  border-radius: 5px; "
        "}"));

    progressStatusLabel_ = new QLabel(sidebar);
    progressStatusLabel_->setObjectName(QStringLiteral("ToolProgressLabel"));
    progressStatusLabel_->setWordWrap(true);
    progressStatusLabel_->setFixedHeight(54);
    progressStatusLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    progressStatusLabel_->setVisible(false);

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(60);
    connect(progressTimer_, &QTimer::timeout, this, &ToolWorkspaceWidget::flushPendingProgress);

    btnCancel_ = new QPushButton(QStringLiteral("CANCEL"), sidebar);
    btnCancel_->setObjectName(QStringLiteral("ToolCancelBtn"));
    btnCancel_->setVisible(false);

    auto* navLabel = new QLabel(QStringLiteral("WORKSPACE SECTIONS"), sidebar);
    navLabel->setObjectName(QStringLiteral("SidebarSectionTitle"));

    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);

    auto makeNavBtn = [&](const QString& text, int idx) {
        auto* btn = new QPushButton(text, sidebar);
        btn->setObjectName(QStringLiteral("WorkspaceNavBtn"));
        btn->setCheckable(true);
        btn->setMinimumHeight(36);
        navGroup_->addButton(btn, idx);
        return btn;
    };

    btnTabGeneral_ = makeNavBtn(QStringLiteral("General Options"), 0);
    btnTabAdvanced_ = makeNavBtn(QStringLiteral("Advanced Options"), 1);
    btnTabSettings_ = makeNavBtn(QStringLiteral("Settings"), 2);
    btnTabGeneral_->setChecked(true);

    sideLayout->addWidget(btnCloseTool_);
    sideLayout->addWidget(btnExecute_);
    sideLayout->addWidget(progressBar_);
    sideLayout->addWidget(progressStatusLabel_);
    sideLayout->addWidget(btnCancel_);
    sideLayout->addSpacing(8);
    sideLayout->addWidget(navLabel);
    sideLayout->addWidget(btnTabGeneral_);
    sideLayout->addWidget(btnTabAdvanced_);
    sideLayout->addWidget(btnTabSettings_);
    sideLayout->addStretch(1);

    // =========================================================================
    // Right Content Area
    // =========================================================================
    auto* rightArea = new QWidget(this);
    rightArea->setObjectName(QStringLiteral("ToolContentArea"));
    auto* rightLayout = new QVBoxLayout(rightArea);
    rightLayout->setContentsMargins(20, 16, 20, 16);
    rightLayout->setSpacing(14);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addStretch(1);
    toolHeaderBadge_ = new QLabel(QStringLiteral("CONVERT COMIC BOOKS"), rightArea);
    toolHeaderBadge_->setObjectName(QStringLiteral("WorkspaceHeaderBadge"));
    headerRow->addWidget(toolHeaderBadge_);
    rightLayout->addLayout(headerRow);

    rightTabStack_ = new QStackedWidget(rightArea);
    rightTabStack_->setObjectName(QStringLiteral("ToolTabStack"));

    // -------------------------------------------------------------------------
    // Tab 0: General Options
    // -------------------------------------------------------------------------
    auto* genPage = new QWidget();
    auto* genLayout = new QVBoxLayout(genPage);
    genLayout->setContentsMargins(4, 4, 8, 12);
    genLayout->setSpacing(16);

    // 1. INPUT OPTIONS
    cardInput_ = new SectionCardWidget(QStringLiteral("INPUT OPTIONS"), genPage);
    inputList_ = new QListWidget(cardInput_);
    inputList_->setMinimumHeight(140);
    inputList_->setSelectionMode(QAbstractItemView::ExtendedSelection);

    auto* inputBtnRow = new QHBoxLayout();
    btnAddFiles_ = new QPushButton(QStringLiteral("ADD FILES"), cardInput_);
    btnAddFolders_ = new QPushButton(QStringLiteral("ADD FOLDERS"), cardInput_);
    btnClearList_ = new QPushButton(QStringLiteral("CLEAR LIST"), cardInput_);
    btnMoveUp_ = new QPushButton(QStringLiteral("MOVE UP"), cardInput_);
    btnMoveDown_ = new QPushButton(QStringLiteral("MOVE DOWN"), cardInput_);
    inputBtnRow->addWidget(btnAddFiles_);
    inputBtnRow->addWidget(btnAddFolders_);
    inputBtnRow->addWidget(btnClearList_);
    inputBtnRow->addStretch(1);
    inputBtnRow->addWidget(btnMoveUp_);
    inputBtnRow->addWidget(btnMoveDown_);

    cardInput_->contentLayout()->addWidget(inputList_);
    cardInput_->contentLayout()->addLayout(inputBtnRow);
    genLayout->addWidget(cardInput_);

    // 2. OUTPUT OPTIONS
    cardOutput_ = new SectionCardWidget(QStringLiteral("OUTPUT OPTIONS"), genPage);
    auto* outForm = new QFormLayout();
    outForm->setSpacing(10);

    cmbOutputFormat_ = new QComboBox(cardOutput_);
    cmbOutputFolderMode_ = new QComboBox(cardOutput_);
    cmbOutputFolderMode_->addItem(QStringLiteral("Same as input folder"), QStringLiteral("same_as_input"));
    cmbOutputFolderMode_->addItem(QStringLiteral("Specify one for all files"), QStringLiteral("custom"));

    auto* outPathRow = new QHBoxLayout();
    editOutputDir_ = new QLineEdit(cardOutput_);
    editOutputDir_->setPlaceholderText(QStringLiteral("Output folder path..."));
    btnChangeOutputDir_ = new QPushButton(QStringLiteral("CHANGE"), cardOutput_);
    btnOpenOutputDir_ = new QPushButton(QStringLiteral("OPEN"), cardOutput_);
    outPathRow->addWidget(editOutputDir_, 1);
    outPathRow->addWidget(btnChangeOutputDir_);
    outPathRow->addWidget(btnOpenOutputDir_);

    outForm->addRow(QStringLiteral("Format:"), cmbOutputFormat_);
    lblOutputFormatHint_ = new QLabel(
        QStringLiteral("Note: WebP/RAR5 sources convert to lossless Zlib PDF streams "
                       "(or select JPG in Image Format for compact PDF size)."),
        cardOutput_);
    lblOutputFormatHint_->setObjectName(QStringLiteral("ToolProgressLabel"));
    lblOutputFormatHint_->setWordWrap(true);
    lblOutputFormatHint_->setVisible(false);
    outForm->addRow(QString(), lblOutputFormatHint_);
    outForm->addRow(QStringLiteral("Output Folder:"), cmbOutputFolderMode_);
    outForm->addRow(QStringLiteral("Folder Path:"), outPathRow);
    lblOutputPathHashHint_ = new QLabel(
        QStringLiteral("Note: Windows 11 Explorer's Recycle Bin has a known issue deleting folders "
                       "inside paths containing '#' (use Shift+Delete or avoid '#' in the output path)."),
        cardOutput_);
    lblOutputPathHashHint_->setObjectName(QStringLiteral("ToolProgressLabel"));
    lblOutputPathHashHint_->setWordWrap(true);
    lblOutputPathHashHint_->setVisible(false);
    outForm->addRow(QString(), lblOutputPathHashHint_);
    cardOutput_->contentLayout()->addLayout(outForm);
    genLayout->addWidget(cardOutput_);

    // 3. IMAGE PROCESSING OPTIONS
    cardImage_ = new SectionCardWidget(QStringLiteral("IMAGE PROCESSING OPTIONS"), genPage);
    auto* imgForm = new QFormLayout();
    imgForm->setSpacing(10);

    auto* scaleRow = new QHBoxLayout();
    sldScale_ = new QSlider(Qt::Horizontal, cardImage_);
    sldScale_->setRange(10, 200);
    sldScale_->setValue(100);
    spnScale_ = new QSpinBox(cardImage_);
    spnScale_->setRange(10, 10000);
    spnScale_->setValue(100);
    cmbScaleMode_ = new QComboBox(cardImage_);
    cmbScaleMode_->addItem(QStringLiteral("Percentage (%)"), QStringLiteral("percentage"));
    cmbScaleMode_->addItem(QStringLiteral("Max Width (px)"), QStringLiteral("max_width"));
    cmbScaleMode_->addItem(QStringLiteral("Max Height (px)"), QStringLiteral("max_height"));
    scaleRow->addWidget(sldScale_, 1);
    scaleRow->addWidget(spnScale_);
    scaleRow->addWidget(cmbScaleMode_);

    cmbTargetImageFormat_ = new QComboBox(cardImage_);
    cmbTargetImageFormat_->addItem(QStringLiteral("Keep the original"), QStringLiteral("original"));
    cmbTargetImageFormat_->addItem(QStringLiteral("JPG"), QStringLiteral("jpg"));
    cmbTargetImageFormat_->addItem(QStringLiteral("PNG"), QStringLiteral("png"));
    cmbTargetImageFormat_->addItem(QStringLiteral("WEBP"), QStringLiteral("webp"));

    imgForm->addRow(QStringLiteral("Scale:"), scaleRow);
    imgForm->addRow(QStringLiteral("Image Format:"), cmbTargetImageFormat_);
    cardImage_->contentLayout()->addLayout(imgForm);
    genLayout->addWidget(cardImage_);

    // 4. OCR EXTRACTED TEXT (shown for extract_ocr)
    cardOcrResult_ = new SectionCardWidget(QStringLiteral("OCR EXTRACTED TEXT"), genPage);
    auto* ocrLangRow = new QHBoxLayout();
    ocrLangRow->addWidget(new QLabel(QStringLiteral("OCR Language:"), cardOcrResult_));
    cmbOcrLanguage_ = new QComboBox(cardOcrResult_);
    cmbOcrLanguage_->addItem(QStringLiteral("Auto (User Profile)"), QStringLiteral("auto"));
    cmbOcrLanguage_->addItem(QStringLiteral("English (en)"), QStringLiteral("en"));
    cmbOcrLanguage_->addItem(QStringLiteral("Japanese (ja)"), QStringLiteral("ja"));
    cmbOcrLanguage_->addItem(QStringLiteral("Indonesian (id)"), QStringLiteral("id"));
    cmbOcrLanguage_->addItem(QStringLiteral("Korean (ko)"), QStringLiteral("ko"));
    cmbOcrLanguage_->addItem(QStringLiteral("Chinese Simplified (zh-Hans)"), QStringLiteral("zh-Hans"));
    ocrLangRow->addWidget(cmbOcrLanguage_);
    ocrLangRow->addStretch(1);

    ocrOutputEdit_ = new QPlainTextEdit(cardOcrResult_);
    ocrOutputEdit_->setMinimumHeight(180);
    ocrOutputEdit_->setPlaceholderText(
        QStringLiteral("Extracted OCR text will appear here after running OCR..."));

    auto* ocrBtnRow = new QHBoxLayout();
    btnCopyOcr_ = new QPushButton(QStringLiteral("COPY TEXT"), cardOcrResult_);
    btnSaveOcr_ = new QPushButton(QStringLiteral("SAVE .TXT"), cardOcrResult_);
    ocrBtnRow->addWidget(btnCopyOcr_);
    ocrBtnRow->addWidget(btnSaveOcr_);
    ocrBtnRow->addStretch(1);

    cardOcrResult_->contentLayout()->addLayout(ocrLangRow);
    cardOcrResult_->contentLayout()->addWidget(ocrOutputEdit_);
    cardOcrResult_->contentLayout()->addLayout(ocrBtnRow);
    cardOcrResult_->setVisible(false);
    genLayout->addWidget(cardOcrResult_);
    genLayout->addStretch(1);

    rightTabStack_->addWidget(wrapInScrollArea(genPage, rightTabStack_));

    // -------------------------------------------------------------------------
    // Tab 1: Advanced Options
    // -------------------------------------------------------------------------
    auto* advPage = new QWidget();
    auto* advLayout = new QVBoxLayout(advPage);
    advLayout->setContentsMargins(4, 4, 8, 12);
    advLayout->setSpacing(16);

    // 1. ADVANCED INPUT OPTIONS
    cardAdvInput_ = new SectionCardWidget(QStringLiteral("ADVANCED INPUT OPTIONS"), advPage);
    extRowContainer_ = new QWidget(cardAdvInput_);
    auto* extRow = new QHBoxLayout(extRowContainer_);
    extRow->setContentsMargins(0, 0, 0, 0);
    chkExtCbz_ = new QCheckBox(QStringLiteral(".cbz"), extRowContainer_);
    chkExtCbr_ = new QCheckBox(QStringLiteral(".cbr"), extRowContainer_);
    chkExtCb7_ = new QCheckBox(QStringLiteral(".cb7"), extRowContainer_);
    chkExtCbt_ = new QCheckBox(QStringLiteral(".cbt"), extRowContainer_);
    chkExtPdf_ = new QCheckBox(QStringLiteral(".pdf"), extRowContainer_);
    chkExtEpub_ = new QCheckBox(QStringLiteral(".epub"), extRowContainer_);
    extRow->addWidget(chkExtCbz_);
    extRow->addWidget(chkExtCbr_);
    extRow->addWidget(chkExtCb7_);
    extRow->addWidget(chkExtCbt_);
    extRow->addWidget(chkExtPdf_);
    extRow->addWidget(chkExtEpub_);
    extRow->addStretch(1);

    lblInputRuleNote_ = new QLabel(cardAdvInput_);
    lblInputRuleNote_->setWordWrap(true);

    cmbCreateFolderMode_ = new QComboBox(cardAdvInput_);
    cmbCreateFolderMode_->addItem(QStringLiteral("Per subfolder (1 comic per folder)"), QStringLiteral("per_subfolder"));
    cmbCreateFolderMode_->addItem(QStringLiteral("Single combined comic"), QStringLiteral("single_combined"));

    cmbAdvOcrLanguage_ = new QComboBox(cardAdvInput_);
    cmbAdvOcrLanguage_->addItem(QStringLiteral("Auto (User Profile)"), QStringLiteral("auto"));
    cmbAdvOcrLanguage_->addItem(QStringLiteral("English (en)"), QStringLiteral("en"));
    cmbAdvOcrLanguage_->addItem(QStringLiteral("Japanese (ja)"), QStringLiteral("ja"));
    cmbAdvOcrLanguage_->addItem(QStringLiteral("Indonesian (id)"), QStringLiteral("id"));
    cmbAdvOcrLanguage_->addItem(QStringLiteral("Korean (ko)"), QStringLiteral("ko"));
    cmbAdvOcrLanguage_->addItem(QStringLiteral("Chinese Simplified (zh-Hans)"), QStringLiteral("zh-Hans"));

    editPageRange_ = new QLineEdit(cardAdvInput_);
    editPageRange_->setPlaceholderText(QStringLiteral("All pages (or e.g. 1-20, 25)"));

    spnOcrUpscale_ = new QDoubleSpinBox(cardAdvInput_);
    spnOcrUpscale_->setRange(1.0, 2.0);
    spnOcrUpscale_->setSingleStep(0.25);
    spnOcrUpscale_->setDecimals(2);
    spnOcrUpscale_->setSuffix(QStringLiteral("x"));

    chkRecursiveFolders_ = new QCheckBox(
        QStringLiteral("Search Input Folders Recursively"), cardAdvInput_);
    spnPdfExtractDpi_ = new QSpinBox(cardAdvInput_);
    spnPdfExtractDpi_->setRange(150, 600);
    spnPdfExtractDpi_->setSingleStep(50);
    spnPdfExtractDpi_->setSuffix(QStringLiteral(" dpi"));
    editArchivePassword_ = new QLineEdit(cardAdvInput_);
    editArchivePassword_->setEchoMode(QLineEdit::Password);
    editArchivePassword_->setPlaceholderText(QStringLiteral("Optional archive password..."));

    advInForm_ = new QFormLayout();
    advInForm_->setSpacing(10);
    advInForm_->addRow(QStringLiteral("Allowed Formats:"), extRowContainer_);
    advInForm_->addRow(QStringLiteral("Input Rule:"), lblInputRuleNote_);
    advInForm_->addRow(QStringLiteral("Folder Packaging:"), cmbCreateFolderMode_);
    advInForm_->addRow(QStringLiteral("OCR Language:"), cmbAdvOcrLanguage_);
    advInForm_->addRow(QStringLiteral("Page Range:"), editPageRange_);
    advInForm_->addRow(QStringLiteral("Small-Text Upscale:"), spnOcrUpscale_);
    advInForm_->addRow(QStringLiteral("PDF Extraction DPI:"), spnPdfExtractDpi_);
    advInForm_->addRow(QStringLiteral("Archive Password:"), editArchivePassword_);
    cardAdvInput_->contentLayout()->addLayout(advInForm_);
    cardAdvInput_->contentLayout()->addWidget(chkRecursiveFolders_);
    advLayout->addWidget(cardAdvInput_);

    // 2. ADVANCED OUTPUT OPTIONS
    cardAdvOutput_ = new SectionCardWidget(QStringLiteral("ADVANCED OUTPUT OPTIONS"), advPage);
    advOutForm_ = new QFormLayout();
    advOutForm_->setSpacing(10);

    cmbIfExists_ = new QComboBox(cardAdvOutput_);
    cmbIfExists_->addItem(QStringLiteral("Rename automatically ( (1), (2) )"), QStringLiteral("rename"));
    cmbIfExists_->addItem(QStringLiteral("Overwrite existing"), QStringLiteral("overwrite"));
    cmbIfExists_->addItem(QStringLiteral("Skip existing"), QStringLiteral("skip"));

    cmbPageNumberDigits_ = new QComboBox(cardAdvOutput_);
    cmbPageNumberDigits_->addItem(QStringLiteral("001 (3 digits, auto 4 for 1000+)"), 3);
    cmbPageNumberDigits_->addItem(QStringLiteral("0001 (4 digits)"), 4);
    cmbPageNumberDigits_->addItem(QStringLiteral("Original internal page names"), 0);

    lblExtractSubfolderNote_ = new QLabel(
        QStringLiteral("Pages are saved into <Destination>/<Comic Original Name>/001.ext"),
        cardAdvOutput_);
    lblExtractSubfolderNote_->setWordWrap(true);

    cmbOcrOutputFormat_ = new QComboBox(cardAdvOutput_);
    cmbOcrOutputFormat_->addItem(QStringLiteral("Plain Text (.txt)"), QStringLiteral("txt"));
    cmbOcrOutputFormat_->addItem(QStringLiteral("Markdown (.md)"), QStringLiteral("md"));
    cmbOcrOutputFormat_->addItem(QStringLiteral("Structured JSON (.json)"), QStringLiteral("json"));

    chkOcrIncludeHeaders_ = new QCheckBox(
        QStringLiteral("Include page headers (=== Page N ===)"), cardAdvOutput_);
    chkOcrFilterNoiseSymbols_ = new QCheckBox(
        QStringLiteral("Filter screentone symbol noise"), cardAdvOutput_);
    chkOcrFilterNoiseSymbols_->setChecked(true);
    chkOcrNormalizeAllCaps_ = new QCheckBox(
        QStringLiteral("Normalize ALL-CAPS text to Sentence case"), cardAdvOutput_);
    chkOcrNormalizeAllCaps_->setChecked(true);

    chkKeepSubfolders_ = new QCheckBox(QStringLiteral("Keep Subfolder Structure"), cardAdvOutput_);
    chkStripMetadata_ = new QCheckBox(
        QStringLiteral("Strip EXIF / XMP / ICC image metadata"), cardAdvOutput_);

    spnSplitFiles_ = new QSpinBox(cardAdvOutput_);
    spnSplitFiles_->setRange(1, 100);

    cmbCompressionMode_ = new QComboBox(cardAdvOutput_);
    cmbCompressionMode_->addItem(QStringLiteral("Store (Zero Recompression)"), QStringLiteral("store"));
    cmbCompressionMode_->addItem(QStringLiteral("Balanced (Deflate 6)"), QStringLiteral("balanced"));
    cmbCompressionMode_->addItem(QStringLiteral("Maximum (Deflate 9)"), QStringLiteral("max"));

    spnPdfCreationDpi_ = new QSpinBox(cardAdvOutput_);
    spnPdfCreationDpi_->setRange(0, 600);
    spnPdfCreationDpi_->setSpecialValueText(QStringLiteral("Auto (Image DPI)"));
    spnPdfCreationDpi_->setSuffix(QStringLiteral(" dpi"));

    chkGenerateBookmarks_ = new QCheckBox(
        QStringLiteral("Auto-Generate Chapter Bookmarks && ComicInfo.xml"), cardAdvOutput_);

    advOutForm_->addRow(QStringLiteral("If Output Already Exists:"), cmbIfExists_);
    advOutForm_->addRow(QStringLiteral("Page Numbering:"), cmbPageNumberDigits_);
    advOutForm_->addRow(QStringLiteral("OCR Export Format:"), cmbOcrOutputFormat_);
    advOutForm_->addRow(QStringLiteral("Number of Files to Split Into:"), spnSplitFiles_);
    advOutForm_->addRow(QStringLiteral("Compression Level:"), cmbCompressionMode_);
    advOutForm_->addRow(QStringLiteral("PDF Creation DPI:"), spnPdfCreationDpi_);
    cardAdvOutput_->contentLayout()->addLayout(advOutForm_);
    cardAdvOutput_->contentLayout()->addWidget(lblExtractSubfolderNote_);
    cardAdvOutput_->contentLayout()->addWidget(chkOcrIncludeHeaders_);
    cardAdvOutput_->contentLayout()->addWidget(chkOcrFilterNoiseSymbols_);
    cardAdvOutput_->contentLayout()->addWidget(chkOcrNormalizeAllCaps_);
    cardAdvOutput_->contentLayout()->addWidget(chkKeepSubfolders_);
    cardAdvOutput_->contentLayout()->addWidget(chkStripMetadata_);
    cardAdvOutput_->contentLayout()->addWidget(chkGenerateBookmarks_);
    advLayout->addWidget(cardAdvOutput_);

    // 3. ADVANCED IMAGE PROCESSING OPTIONS
    cardAdvImage_ = new SectionCardWidget(QStringLiteral("ADVANCED IMAGE PROCESSING OPTIONS"), advPage);
    chkMergeNormalizeWidth_ = new QCheckBox(
        QStringLiteral("Normalize all merged pages to matching width"), cardAdvImage_);
    cardAdvImage_->contentLayout()->addWidget(chkMergeNormalizeWidth_);
    auto* advImgGrid = new QGridLayout();
    advImgGrid->setSpacing(10);

    chkEnableLevels_ = new QCheckBox(QStringLiteral("White / Black Level"), cardAdvImage_);
    spnBlackLevel_ = new QSpinBox(cardAdvImage_);
    spnBlackLevel_->setRange(0, 254);
    spnBlackLevel_->setPrefix(QStringLiteral("Black: "));
    spnWhiteLevel_ = new QSpinBox(cardAdvImage_);
    spnWhiteLevel_->setRange(1, 255);
    spnWhiteLevel_->setPrefix(QStringLiteral("White: "));

    spnBrightness_ = new QSpinBox(cardAdvImage_);
    spnBrightness_->setRange(-100, 100);
    spnSharpness_ = new QSpinBox(cardAdvImage_);
    spnSharpness_->setRange(0, 100);
    chkGrayscaleManga_ = new QCheckBox(QStringLiteral("Grayscale Manga Optimize"), cardAdvImage_);

    spnCropBorderPx_ = new QSpinBox(cardAdvImage_);
    spnCropBorderPx_->setRange(0, 500);
    spnCropBorderPx_->setSuffix(QStringLiteral(" px"));
    chkAutoCropUniform_ = new QCheckBox(QStringLiteral("Auto-Crop Uniform Borders"), cardAdvImage_);

    spnAddBorderPx_ = new QSpinBox(cardAdvImage_);
    spnAddBorderPx_->setRange(0, 500);
    spnAddBorderPx_->setSuffix(QStringLiteral(" px"));
    btnBorderColor_ = new QPushButton(QStringLiteral("#000000"), cardAdvImage_);
    btnResetImageOps_ = new QPushButton(QStringLiteral("RESET IMAGE OPS"), cardAdvImage_);

    spnWorkerThreads_ = new QSpinBox(cardAdvImage_);
    spnWorkerThreads_->setRange(1, 16);

    sldJpgQuality_ = new QSlider(Qt::Horizontal, cardAdvImage_);
    sldJpgQuality_->setRange(1, 100);
    spnJpgQuality_ = new QSpinBox(cardAdvImage_);
    spnJpgQuality_->setRange(1, 100);

    sldPngCompression_ = new QSlider(Qt::Horizontal, cardAdvImage_);
    sldPngCompression_->setRange(0, 9);
    spnPngCompression_ = new QSpinBox(cardAdvImage_);
    spnPngCompression_->setRange(0, 9);

    sldWebpQuality_ = new QSlider(Qt::Horizontal, cardAdvImage_);
    sldWebpQuality_->setRange(1, 100);
    spnWebpQuality_ = new QSpinBox(cardAdvImage_);
    spnWebpQuality_->setRange(1, 100);
    chkWebpLossless_ = new QCheckBox(QStringLiteral("Lossless WebP"), cardAdvImage_);

    int row = 0;
    advImgGrid->addWidget(chkEnableLevels_, row, 0);
    advImgGrid->addWidget(spnBlackLevel_, row, 1);
    advImgGrid->addWidget(spnWhiteLevel_, row, 2);
    ++row;
    lblBrightness_ = new QLabel(QStringLiteral("Brightness:"), cardAdvImage_);
    advImgGrid->addWidget(lblBrightness_, row, 0);
    advImgGrid->addWidget(spnBrightness_, row, 1);
    advImgGrid->addWidget(chkGrayscaleManga_, row, 2);
    ++row;
    lblSharpness_ = new QLabel(QStringLiteral("Ink Sharpness:"), cardAdvImage_);
    advImgGrid->addWidget(lblSharpness_, row, 0);
    advImgGrid->addWidget(spnSharpness_, row, 1);
    advImgGrid->addWidget(btnResetImageOps_, row, 2);
    ++row;
    lblCropBorder_ = new QLabel(QStringLiteral("Crop Border:"), cardAdvImage_);
    advImgGrid->addWidget(lblCropBorder_, row, 0);
    advImgGrid->addWidget(spnCropBorderPx_, row, 1);
    advImgGrid->addWidget(chkAutoCropUniform_, row, 2);
    ++row;
    lblAddBorder_ = new QLabel(QStringLiteral("Add Border:"), cardAdvImage_);
    advImgGrid->addWidget(lblAddBorder_, row, 0);
    advImgGrid->addWidget(spnAddBorderPx_, row, 1);
    advImgGrid->addWidget(btnBorderColor_, row, 2);
    ++row;
    advImgGrid->addWidget(new QLabel(QStringLiteral("Worker Threads:"), cardAdvImage_), row, 0);
    advImgGrid->addWidget(spnWorkerThreads_, row, 1);
    ++row;
    advImgGrid->addWidget(new QLabel(QStringLiteral("JPG Quality:"), cardAdvImage_), row, 0);
    advImgGrid->addWidget(sldJpgQuality_, row, 1);
    advImgGrid->addWidget(spnJpgQuality_, row, 2);
    ++row;
    advImgGrid->addWidget(new QLabel(QStringLiteral("PNG Compression:"), cardAdvImage_), row, 0);
    advImgGrid->addWidget(sldPngCompression_, row, 1);
    advImgGrid->addWidget(spnPngCompression_, row, 2);
    ++row;
    advImgGrid->addWidget(new QLabel(QStringLiteral("WebP Quality:"), cardAdvImage_), row, 0);
    advImgGrid->addWidget(sldWebpQuality_, row, 1);
    advImgGrid->addWidget(spnWebpQuality_, row, 2);
    ++row;
    advImgGrid->addWidget(chkWebpLossless_, row, 1, 1, 2);

    cardAdvImage_->contentLayout()->addLayout(advImgGrid);
    advLayout->addWidget(cardAdvImage_);
    advLayout->addStretch(1);

    rightTabStack_->addWidget(wrapInScrollArea(advPage, rightTabStack_));

    // -------------------------------------------------------------------------
    // Tab 2: Settings
    // -------------------------------------------------------------------------
    auto* setPage = new QWidget();
    auto* setLayout = new QVBoxLayout(setPage);
    setLayout->setContentsMargins(4, 4, 8, 12);
    setLayout->setSpacing(16);

    cardToolSettings_ = new SectionCardWidget(QStringLiteral("SETTINGS"), setPage);
    btnResetToolOptions_ = new QPushButton(QStringLiteral("RESET OPTIONS"), cardToolSettings_);
    btnResetToolOptions_->setMinimumHeight(38);
    chkRememberLastOptions_ = new QCheckBox(
        QStringLiteral("Remember Last Used Options for this tool"), cardToolSettings_);

    cardToolSettings_->contentLayout()->addWidget(btnResetToolOptions_);
    cardToolSettings_->contentLayout()->addWidget(chkRememberLastOptions_);
    setLayout->addWidget(cardToolSettings_);
    setLayout->addStretch(1);

    rightTabStack_->addWidget(wrapInScrollArea(setPage, rightTabStack_));

    rightLayout->addWidget(rightTabStack_, 1);

    rootLayout->addWidget(sidebar);
    rootLayout->addWidget(rightArea, 1);

    // =========================================================================
    // Signal Wiring
    // =========================================================================
    connect(navGroup_, &QButtonGroup::idClicked, rightTabStack_, &QStackedWidget::setCurrentIndex);
    connect(btnCloseTool_, &QPushButton::clicked, this, &ToolWorkspaceWidget::closeTool);
    connect(btnExecute_, &QPushButton::clicked, this, &ToolWorkspaceWidget::executeCurrentTool);
    connect(btnCancel_, &QPushButton::clicked, this, &ToolWorkspaceWidget::cancelExecution);

    connect(btnAddFiles_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onAddFiles);
    connect(btnAddFolders_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onAddFolders);
    connect(btnClearList_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onClearList);
    connect(btnMoveUp_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onMoveUp);
    connect(btnMoveDown_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onMoveDown);

    connect(btnChangeOutputDir_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onBrowseOutputDir);
    connect(btnOpenOutputDir_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onOpenOutputDir);
    auto updateHashHint = [this]() {
        if (lblOutputPathHashHint_ == nullptr) return;
        const bool isCustom =
            cmbOutputFolderMode_->currentData().toString() == QStringLiteral("custom");
        bool hasHash = false;
        if (isCustom) {
            hasHash = editOutputDir_->text().contains(QLatin1Char('#'));
        } else if (inputList_->count() > 0) {
            hasHash = inputList_->item(0)->text().contains(QLatin1Char('#'));
        }
        lblOutputPathHashHint_->setVisible(hasHash);
    };
    connect(cmbOutputFolderMode_, &QComboBox::currentIndexChanged, this, [this, updateHashHint](int) {
        const bool isCustom =
            cmbOutputFolderMode_->currentData().toString() == QStringLiteral("custom");
        editOutputDir_->setEnabled(isCustom);
        btnChangeOutputDir_->setEnabled(isCustom);
        updateHashHint();
    });
    connect(editOutputDir_, &QLineEdit::textChanged, this, [updateHashHint](const QString&) {
        updateHashHint();
    });

    connect(sldScale_, &QSlider::valueChanged, this, [this](int v) {
        if (spnScale_->value() != v) spnScale_->setValue(v);
    });
    connect(spnScale_, &QSpinBox::valueChanged, this, [this](int v) {
        if (v <= 200 && sldScale_->value() != v) sldScale_->setValue(v);
    });
    connect(cmbScaleMode_, &QComboBox::currentIndexChanged, this, [this](int) {
        const bool isPct =
            cmbScaleMode_->currentData().toString() == QStringLiteral("percentage");
        sldScale_->setEnabled(isPct);
        spnScale_->setRange(isPct ? 10 : 64, isPct ? 200 : 10000);
        if (isPct && spnScale_->value() > 200) {
            spnScale_->setValue(100);
        }
    });

    connect(sldJpgQuality_, &QSlider::valueChanged, spnJpgQuality_, &QSpinBox::setValue);
    connect(spnJpgQuality_, &QSpinBox::valueChanged, sldJpgQuality_, &QSlider::setValue);
    connect(sldPngCompression_, &QSlider::valueChanged, spnPngCompression_, &QSpinBox::setValue);
    connect(spnPngCompression_, &QSpinBox::valueChanged, sldPngCompression_, &QSlider::setValue);
    connect(sldWebpQuality_, &QSlider::valueChanged, spnWebpQuality_, &QSpinBox::setValue);
    connect(spnWebpQuality_, &QSpinBox::valueChanged, sldWebpQuality_, &QSlider::setValue);

    connect(btnBorderColor_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onPickBorderColor);
    connect(btnResetImageOps_, &QPushButton::clicked, this, &ToolWorkspaceWidget::resetImageOpsOnly);
    connect(btnResetToolOptions_, &QPushButton::clicked, this, &ToolWorkspaceWidget::resetAllToolOptions);
    connect(btnCopyOcr_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onCopyOcrText);
    connect(btnSaveOcr_, &QPushButton::clicked, this, &ToolWorkspaceWidget::onSaveOcrText);

    connect(cmbOcrLanguage_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (cmbAdvOcrLanguage_->currentIndex() != idx) {
            cmbAdvOcrLanguage_->setCurrentIndex(idx);
        }
    });
    connect(cmbAdvOcrLanguage_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (cmbOcrLanguage_->currentIndex() != idx) {
            cmbOcrLanguage_->setCurrentIndex(idx);
        }
    });
    connect(cmbOutputFormat_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QString fmt = cmbOutputFormat_->currentData().toString();
        if (lblOutputFormatHint_ != nullptr) {
            lblOutputFormatHint_->setVisible(fmt == QStringLiteral("pdf"));
        }
        if (currentToolId_ == QStringLiteral("extract_ocr")) {
            const int i = cmbOcrOutputFormat_->findData(fmt);
            if (i >= 0 && cmbOcrOutputFormat_->currentIndex() != i) {
                cmbOcrOutputFormat_->setCurrentIndex(i);
            }
        }
    });
    connect(cmbOcrOutputFormat_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (currentToolId_ == QStringLiteral("extract_ocr")) {
            const int i = cmbOutputFormat_->findData(cmbOcrOutputFormat_->currentData());
            if (i >= 0 && cmbOutputFormat_->currentIndex() != i) {
                cmbOutputFormat_->setCurrentIndex(i);
            }
        }
    });
}

void ToolWorkspaceWidget::configureForTool(const QString& toolId) {
    currentToolId_ = toolId;
    cmbOutputFormat_->clear();

    const bool isOrderable = (toolId == QStringLiteral("merge_comics") ||
                              toolId == QStringLiteral("create_comic"));
    btnMoveUp_->setVisible(isOrderable);
    btnMoveDown_->setVisible(isOrderable);
    cardOcrResult_->setVisible(toolId == QStringLiteral("extract_ocr"));
    cardImage_->setVisible(toolId != QStringLiteral("extract_ocr"));

    if (toolId == QStringLiteral("convert_comics")) {
        toolHeaderBadge_->setText(LocalizationManager::tr("tool.convert_comics").toUpper());
        btnExecute_->setText(LocalizationManager::tr("tool.btn.convert"));
        cmbOutputFormat_->addItem(QStringLiteral("CBZ (.cbz)"), QStringLiteral("cbz"));
        cmbOutputFormat_->addItem(QStringLiteral("CB7 (.cb7 - 7-Zip)"), QStringLiteral("cb7"));
        cmbOutputFormat_->addItem(QStringLiteral("CBT (.cbt - TAR)"), QStringLiteral("cbt"));
        cmbOutputFormat_->addItem(QStringLiteral("ZIP (.zip)"), QStringLiteral("zip"));
        cmbOutputFormat_->addItem(QStringLiteral("EPUB (.epub - Fixed Layout)"), QStringLiteral("epub"));
        cmbOutputFormat_->addItem(QStringLiteral("PDF (.pdf)"), QStringLiteral("pdf"));
    } else if (toolId == QStringLiteral("convert_images")) {
        toolHeaderBadge_->setText(LocalizationManager::tr("tool.convert_images").toUpper());
        btnExecute_->setText(LocalizationManager::tr("tool.btn.convert"));
        cmbOutputFormat_->addItem(QStringLiteral("JPG (.jpg)"), QStringLiteral("jpg"));
        cmbOutputFormat_->addItem(QStringLiteral("PNG (.png)"), QStringLiteral("png"));
        cmbOutputFormat_->addItem(QStringLiteral("WEBP (.webp)"), QStringLiteral("webp"));
    } else if (toolId == QStringLiteral("create_comic")) {
        toolHeaderBadge_->setText(LocalizationManager::tr("tool.create_comic").toUpper());
        btnExecute_->setText(LocalizationManager::tr("tool.btn.create"));
        cmbOutputFormat_->addItem(QStringLiteral("CBZ (.cbz)"), QStringLiteral("cbz"));
        cmbOutputFormat_->addItem(QStringLiteral("CB7 (.cb7 - 7-Zip)"), QStringLiteral("cb7"));
        cmbOutputFormat_->addItem(QStringLiteral("CBT (.cbt - TAR)"), QStringLiteral("cbt"));
        cmbOutputFormat_->addItem(QStringLiteral("ZIP (.zip)"), QStringLiteral("zip"));
        cmbOutputFormat_->addItem(QStringLiteral("EPUB (.epub - Fixed Layout)"), QStringLiteral("epub"));
        cmbOutputFormat_->addItem(QStringLiteral("PDF (.pdf)"), QStringLiteral("pdf"));
    } else if (toolId == QStringLiteral("extract_pages")) {
        toolHeaderBadge_->setText(LocalizationManager::tr("tool.extract_pages").toUpper());
        btnExecute_->setText(LocalizationManager::tr("tool.btn.extract"));
        cmbOutputFormat_->addItem(QStringLiteral("Original Image Format"), QStringLiteral("jpg"));
        cmbOutputFormat_->addItem(QStringLiteral("PNG (.png)"), QStringLiteral("png"));
        cmbOutputFormat_->addItem(QStringLiteral("WEBP (.webp)"), QStringLiteral("webp"));
    } else if (toolId == QStringLiteral("extract_ocr")) {
        toolHeaderBadge_->setText(LocalizationManager::tr("tool.extract_ocr_badge").toUpper());
        btnExecute_->setText(LocalizationManager::tr("tool.btn.run_ocr"));
        cmbOutputFormat_->addItem(QStringLiteral("Plain Text (.txt)"), QStringLiteral("txt"));
        cmbOutputFormat_->addItem(QStringLiteral("Markdown (.md)"), QStringLiteral("md"));
        cmbOutputFormat_->addItem(QStringLiteral("Structured JSON (.json)"), QStringLiteral("json"));
    } else {
        toolHeaderBadge_->setText(LocalizationManager::tr("tool.merge_comics").toUpper());
        btnExecute_->setText(LocalizationManager::tr("tool.btn.merge"));
        cmbOutputFormat_->addItem(QStringLiteral("CBZ (.cbz)"), QStringLiteral("cbz"));
        cmbOutputFormat_->addItem(QStringLiteral("CB7 (.cb7 - 7-Zip)"), QStringLiteral("cb7"));
        cmbOutputFormat_->addItem(QStringLiteral("CBT (.cbt - TAR)"), QStringLiteral("cbt"));
        cmbOutputFormat_->addItem(QStringLiteral("ZIP (.zip)"), QStringLiteral("zip"));
        cmbOutputFormat_->addItem(QStringLiteral("EPUB (.epub - Fixed Layout)"), QStringLiteral("epub"));
        cmbOutputFormat_->addItem(QStringLiteral("PDF (.pdf)"), QStringLiteral("pdf"));
    }
    if (lblOutputFormatHint_ != nullptr) {
        lblOutputFormatHint_->setVisible(cmbOutputFormat_->currentData().toString() == QStringLiteral("pdf"));
    }

    // Tailor Advanced Options cards & controls uniquely per tool
    const bool isConvertComics = (toolId == QStringLiteral("convert_comics"));
    const bool isConvertImages = (toolId == QStringLiteral("convert_images"));
    const bool isCreateComic = (toolId == QStringLiteral("create_comic"));
    const bool isExtractPages = (toolId == QStringLiteral("extract_pages"));
    const bool isExtractOcr = (toolId == QStringLiteral("extract_ocr"));
    const bool isMergeComics = (toolId == QStringLiteral("merge_comics"));

    // 1. Advanced Input Card
    cardAdvInput_->setTabTitle(
        isExtractOcr ? QStringLiteral("ADVANCED OCR INPUT")
                     : (isMergeComics ? QStringLiteral("ADVANCED MERGE INPUT")
                                      : QStringLiteral("ADVANCED INPUT OPTIONS")));
    advInForm_->setRowVisible(extRowContainer_, isConvertComics);
    advInForm_->setRowVisible(lblInputRuleNote_, isConvertImages || isCreateComic || isMergeComics);
    if (isConvertImages) {
        lblInputRuleNote_->setText(QStringLiteral("Supports JPG, PNG, WebP, BMP, GIF, AVIF, and TIFF images."));
    } else if (isCreateComic) {
        lblInputRuleNote_->setText(QStringLiteral("Pages are sorted in natural reading order (1, 2, ..., 10)."));
    } else if (isMergeComics) {
        lblInputRuleNote_->setText(QStringLiteral("Volumes follow manual list order (Move Up/Down); folders sort naturally."));
    }
    advInForm_->setRowVisible(cmbCreateFolderMode_, isCreateComic);
    advInForm_->setRowVisible(cmbAdvOcrLanguage_, isExtractOcr);
    advInForm_->setRowVisible(editPageRange_, isExtractPages || isExtractOcr);
    advInForm_->setRowVisible(spnOcrUpscale_, isExtractOcr);
    advInForm_->setRowVisible(spnPdfExtractDpi_, isConvertComics || isExtractPages || isExtractOcr);
    advInForm_->setRowVisible(editArchivePassword_, isConvertComics || isExtractPages || isMergeComics);
    chkRecursiveFolders_->setVisible(true);

    // 2. Advanced Output Card
    cardAdvOutput_->setTabTitle(
        isExtractOcr ? QStringLiteral("ADVANCED OCR OUTPUT")
                     : (isMergeComics ? QStringLiteral("ADVANCED MERGE OUTPUT")
                                      : QStringLiteral("ADVANCED OUTPUT OPTIONS")));
    advOutForm_->setRowVisible(cmbIfExists_, true);
    advOutForm_->setRowVisible(cmbPageNumberDigits_, isExtractPages);
    advOutForm_->setRowVisible(cmbOcrOutputFormat_, isExtractOcr);
    advOutForm_->setRowVisible(spnSplitFiles_, isConvertComics || isCreateComic || isMergeComics);
    advOutForm_->setRowVisible(cmbCompressionMode_, isConvertComics || isCreateComic || isMergeComics);
    advOutForm_->setRowVisible(spnPdfCreationDpi_, isConvertComics || isCreateComic || isMergeComics);

    lblExtractSubfolderNote_->setVisible(isExtractPages);
    chkOcrIncludeHeaders_->setVisible(isExtractOcr);
    chkOcrFilterNoiseSymbols_->setVisible(isExtractOcr);
    chkOcrNormalizeAllCaps_->setVisible(isExtractOcr);
    chkKeepSubfolders_->setVisible(isConvertComics || isConvertImages || isCreateComic);
    chkStripMetadata_->setVisible(isConvertImages);
    chkGenerateBookmarks_->setVisible(isConvertComics || isCreateComic || isMergeComics);
    if (isConvertComics) {
        chkGenerateBookmarks_->setText(QStringLiteral("Preserve ComicInfo.xml && Chapter Bookmarks"));
    } else if (isCreateComic) {
        chkGenerateBookmarks_->setText(QStringLiteral("Embed ComicInfo.xml Metadata"));
    } else if (isMergeComics) {
        chkGenerateBookmarks_->setText(QStringLiteral("Auto-Generate Volume Chapter Bookmarks && ComicInfo.xml"));
    }

    // 3. Advanced Image Processing / Normalization Card
    cardAdvImage_->setVisible(!isExtractOcr);
    cardAdvImage_->setTabTitle(
        isMergeComics ? QStringLiteral("ADVANCED IMAGE NORMALIZATION")
                      : QStringLiteral("ADVANCED IMAGE PROCESSING OPTIONS"));
    chkMergeNormalizeWidth_->setVisible(isMergeComics);

    const bool showFullImageOps = !isMergeComics;
    chkEnableLevels_->setVisible(showFullImageOps);
    spnBlackLevel_->setVisible(showFullImageOps);
    spnWhiteLevel_->setVisible(showFullImageOps);
    lblBrightness_->setVisible(showFullImageOps);
    spnBrightness_->setVisible(showFullImageOps);
    chkGrayscaleManga_->setVisible(showFullImageOps);
    lblSharpness_->setVisible(showFullImageOps);
    spnSharpness_->setVisible(showFullImageOps);
    btnResetImageOps_->setVisible(showFullImageOps);
    lblCropBorder_->setVisible(showFullImageOps);
    spnCropBorderPx_->setVisible(showFullImageOps);
    chkAutoCropUniform_->setVisible(showFullImageOps);
    lblAddBorder_->setVisible(showFullImageOps);
    spnAddBorderPx_->setVisible(showFullImageOps);
    btnBorderColor_->setVisible(showFullImageOps);
    chkWebpLossless_->setVisible(isConvertImages || isConvertComics || isCreateComic || isExtractPages);
}

void ToolWorkspaceWidget::openTool(
    const QString& toolId,
    const std::vector<std::filesystem::path>& initialPaths) {
    if (isRunning_.load()) {
        if (toolId != currentToolId_) {
            QMessageBox::information(
                this,
                QStringLiteral("Job Running"),
                QStringLiteral("A background tool job is currently running. "
                               "Please cancel or wait for it to finish before switching tools."));
        }
        return;
    }

    resetProgressState();
    configureForTool(toolId);

    mavira::core::ToolExecutionOptions opts;
    if (settingsManager_ != nullptr) {
        if (auto res = settingsManager_->loadToolOptions(toolId.toStdString()); res) {
            opts = res.value();
        }
    }
    if (!initialPaths.empty()) {
        opts.inputPaths.clear();
        for (const auto& p : initialPaths) {
            opts.inputPaths.push_back(p.native());
        }
    }
    applyOptions(opts);
    btnTabGeneral_->setChecked(true);
    rightTabStack_->setCurrentIndex(0);
}

void ToolWorkspaceWidget::closeTool() {
    if (isRunning_.load()) {
        cancelExecution();
    } else {
        resetProgressState();
    }
    persistCurrentOptions();
    emit closeRequested();
}

void ToolWorkspaceWidget::persistCurrentOptions() {
    if (settingsManager_ == nullptr) {
        return;
    }
    (void)settingsManager_->saveToolOptions(currentToolId_.toStdString(), gatherOptions());
}

mavira::core::ToolExecutionOptions ToolWorkspaceWidget::gatherOptions() const {
    mavira::core::ToolExecutionOptions opts;

    for (int i = 0; i < inputList_->count(); ++i) {
        opts.inputPaths.push_back(QDir::toNativeSeparators(inputList_->item(i)->text()).toStdWString());
    }
    opts.recursiveFolders = chkRecursiveFolders_->isChecked();
    opts.allowedExtensions.clear();
    if (chkExtCbz_->isChecked()) opts.allowedExtensions.push_back("cbz");
    if (chkExtCbr_->isChecked()) opts.allowedExtensions.push_back("cbr");
    if (chkExtCb7_->isChecked()) opts.allowedExtensions.push_back("cb7");
    if (chkExtCbt_->isChecked()) opts.allowedExtensions.push_back("cbt");
    if (chkExtPdf_->isChecked()) opts.allowedExtensions.push_back("pdf");
    if (chkExtEpub_->isChecked()) opts.allowedExtensions.push_back("epub");
    opts.archivePassword = editArchivePassword_->text().toStdString();
    opts.pdfExtractDpi = spnPdfExtractDpi_->value();

    opts.outputFormat = cmbOutputFormat_->currentData().toString().toStdString();
    opts.outputFolderMode = cmbOutputFolderMode_->currentData().toString().toStdString();
    opts.customOutputDir = QDir::toNativeSeparators(editOutputDir_->text().trimmed()).toStdWString();
    opts.ifExistsPolicy = cmbIfExists_->currentData().toString().toStdString();
    opts.keepSubfolderStructure = chkKeepSubfolders_->isChecked();
    opts.splitIntoFiles = spnSplitFiles_->value();
    opts.compressionMode = cmbCompressionMode_->currentData().toString().toStdString();
    opts.pdfCreationDpi = spnPdfCreationDpi_->value();
    opts.generateChapterBookmarks = chkGenerateBookmarks_->isChecked();

    opts.scaleMode = cmbScaleMode_->currentData().toString().toStdString();
    opts.scaleValue = spnScale_->value();
    opts.targetImageFormat = cmbTargetImageFormat_->currentData().toString().toStdString();
    opts.enableLevels = chkEnableLevels_->isChecked();
    opts.blackLevel = spnBlackLevel_->value();
    opts.whiteLevel = spnWhiteLevel_->value();
    opts.brightness = spnBrightness_->value();
    opts.sharpness = spnSharpness_->value();
    opts.grayscaleManga = chkGrayscaleManga_->isChecked();
    opts.cropBorderPx = spnCropBorderPx_->value();
    opts.autoCropUniform = chkAutoCropUniform_->isChecked();
    opts.addBorderPx = spnAddBorderPx_->value();
    opts.borderColorHex = borderColorHex_;
    opts.workerThreads = spnWorkerThreads_->value();
    opts.jpgQuality = spnJpgQuality_->value();
    opts.pngCompression = spnPngCompression_->value();
    opts.webpQuality = spnWebpQuality_->value();
    opts.webpLossless = chkWebpLossless_->isChecked();

    opts.pageRange = editPageRange_->text().trimmed().toStdString();
    opts.pageNumberDigits = cmbPageNumberDigits_->currentData().toInt();
    opts.createFolderMode = cmbCreateFolderMode_->currentData().toString().toStdString();
    opts.stripImageMetadata = chkStripMetadata_->isChecked();
    opts.ocrLanguage = cmbAdvOcrLanguage_->currentData().toString().toStdString();
    opts.ocrUpscaleFactor = spnOcrUpscale_->value();
    opts.ocrOutputFormat = cmbOcrOutputFormat_->currentData().toString().toStdString();
    opts.ocrIncludePageHeaders = chkOcrIncludeHeaders_->isChecked();
    opts.ocrFilterNoiseSymbols = chkOcrFilterNoiseSymbols_->isChecked();
    opts.ocrNormalizeAllCaps = chkOcrNormalizeAllCaps_->isChecked();
    opts.mergeNormalizeWidth = chkMergeNormalizeWidth_->isChecked();

    opts.rememberLastOptions = chkRememberLastOptions_->isChecked();
    return opts;
}

void ToolWorkspaceWidget::applyOptions(const mavira::core::ToolExecutionOptions& opts) {
    inputList_->clear();
    for (const auto& wpath : opts.inputPaths) {
        inputList_->addItem(QDir::toNativeSeparators(QString::fromStdWString(wpath)));
    }

    auto hasExt = [&](const std::string& e) {
        return std::find(opts.allowedExtensions.begin(), opts.allowedExtensions.end(), e) !=
               opts.allowedExtensions.end();
    };
    chkExtCbz_->setChecked(hasExt("cbz"));
    chkExtCbr_->setChecked(hasExt("cbr"));
    chkExtCb7_->setChecked(hasExt("cb7"));
    chkExtCbt_->setChecked(hasExt("cbt"));
    chkExtPdf_->setChecked(hasExt("pdf"));
    chkExtEpub_->setChecked(hasExt("epub"));
    chkRecursiveFolders_->setChecked(opts.recursiveFolders);
    spnPdfExtractDpi_->setValue(std::clamp(opts.pdfExtractDpi, 150, 600));
    editArchivePassword_->setText(QString::fromStdString(opts.archivePassword));

    const int fmtIdx = cmbOutputFormat_->findData(QString::fromStdString(opts.outputFormat));
    if (fmtIdx >= 0) cmbOutputFormat_->setCurrentIndex(fmtIdx);

    const int modeIdx = cmbOutputFolderMode_->findData(QString::fromStdString(opts.outputFolderMode));
    cmbOutputFolderMode_->setCurrentIndex(modeIdx >= 0 ? modeIdx : 0);
    editOutputDir_->setText(QDir::toNativeSeparators(QString::fromStdWString(opts.customOutputDir)));
    const bool isCustom =
        cmbOutputFolderMode_->currentData().toString() == QStringLiteral("custom");
    editOutputDir_->setEnabled(isCustom);
    btnChangeOutputDir_->setEnabled(isCustom);

    const int existsIdx = cmbIfExists_->findData(QString::fromStdString(opts.ifExistsPolicy));
    cmbIfExists_->setCurrentIndex(existsIdx >= 0 ? existsIdx : 0);
    chkKeepSubfolders_->setChecked(opts.keepSubfolderStructure);
    spnSplitFiles_->setValue(std::clamp(opts.splitIntoFiles, 1, 100));
    const int compIdx = cmbCompressionMode_->findData(QString::fromStdString(opts.compressionMode));
    cmbCompressionMode_->setCurrentIndex(compIdx >= 0 ? compIdx : 0);
    spnPdfCreationDpi_->setValue(std::clamp(opts.pdfCreationDpi, 0, 600));
    chkGenerateBookmarks_->setChecked(opts.generateChapterBookmarks);

    const int scaleIdx = cmbScaleMode_->findData(QString::fromStdString(opts.scaleMode));
    cmbScaleMode_->setCurrentIndex(scaleIdx >= 0 ? scaleIdx : 0);
    const bool isPct = (opts.scaleMode == "percentage");
    spnScale_->setRange(isPct ? 10 : 64, isPct ? 200 : 10000);
    spnScale_->setValue(opts.scaleValue);
    if (isPct) sldScale_->setValue(std::clamp(opts.scaleValue, 10, 200));

    const int imgFmtIdx =
        cmbTargetImageFormat_->findData(QString::fromStdString(opts.targetImageFormat));
    cmbTargetImageFormat_->setCurrentIndex(imgFmtIdx >= 0 ? imgFmtIdx : 0);

    chkEnableLevels_->setChecked(opts.enableLevels);
    spnBlackLevel_->setValue(std::clamp(opts.blackLevel, 0, 254));
    spnWhiteLevel_->setValue(std::clamp(opts.whiteLevel, 1, 255));
    spnBrightness_->setValue(std::clamp(opts.brightness, -100, 100));
    spnSharpness_->setValue(std::clamp(opts.sharpness, 0, 100));
    chkGrayscaleManga_->setChecked(opts.grayscaleManga);
    spnCropBorderPx_->setValue(std::clamp(opts.cropBorderPx, 0, 500));
    chkAutoCropUniform_->setChecked(opts.autoCropUniform);
    spnAddBorderPx_->setValue(std::clamp(opts.addBorderPx, 0, 500));
    borderColorHex_ = opts.borderColorHex.empty() ? "#000000" : opts.borderColorHex;
    btnBorderColor_->setText(QString::fromStdString(borderColorHex_));
    spnWorkerThreads_->setValue(std::clamp(opts.workerThreads, 1, 16));
    spnJpgQuality_->setValue(std::clamp(opts.jpgQuality, 1, 100));
    spnPngCompression_->setValue(std::clamp(opts.pngCompression, 0, 9));
    spnWebpQuality_->setValue(std::clamp(opts.webpQuality, 1, 100));
    chkWebpLossless_->setChecked(opts.webpLossless);

    editPageRange_->setText(QString::fromStdString(opts.pageRange));
    const int digitsIdx = cmbPageNumberDigits_->findData(opts.pageNumberDigits);
    cmbPageNumberDigits_->setCurrentIndex(digitsIdx >= 0 ? digitsIdx : 0);
    const int folderModeIdx = cmbCreateFolderMode_->findData(QString::fromStdString(opts.createFolderMode));
    cmbCreateFolderMode_->setCurrentIndex(folderModeIdx >= 0 ? folderModeIdx : 0);
    chkStripMetadata_->setChecked(opts.stripImageMetadata);
    const QString ocrLang = opts.ocrLanguage.empty()
        ? QStringLiteral("auto")
        : QString::fromStdString(opts.ocrLanguage);
    const int ocrLangIdx = cmbAdvOcrLanguage_->findData(ocrLang);
    cmbAdvOcrLanguage_->setCurrentIndex(ocrLangIdx >= 0 ? ocrLangIdx : 0);
    cmbOcrLanguage_->setCurrentIndex(ocrLangIdx >= 0 ? ocrLangIdx : 0);
    spnOcrUpscale_->setValue(std::clamp(opts.ocrUpscaleFactor, 1.0, 2.0));
    const int ocrFmtIdx = cmbOcrOutputFormat_->findData(QString::fromStdString(opts.ocrOutputFormat));
    cmbOcrOutputFormat_->setCurrentIndex(ocrFmtIdx >= 0 ? ocrFmtIdx : 0);
    if (currentToolId_ == QStringLiteral("extract_ocr") && ocrFmtIdx >= 0) {
        const int genFmtIdx = cmbOutputFormat_->findData(cmbOcrOutputFormat_->currentData());
        if (genFmtIdx >= 0) cmbOutputFormat_->setCurrentIndex(genFmtIdx);
    }
    chkOcrIncludeHeaders_->setChecked(opts.ocrIncludePageHeaders);
    chkOcrFilterNoiseSymbols_->setChecked(opts.ocrFilterNoiseSymbols);
    chkOcrNormalizeAllCaps_->setChecked(opts.ocrNormalizeAllCaps);
    chkMergeNormalizeWidth_->setChecked(opts.mergeNormalizeWidth);

    chkRememberLastOptions_->setChecked(opts.rememberLastOptions);
}

void ToolWorkspaceWidget::resetProgressState() {
    if (progressTimer_ != nullptr) {
        progressTimer_->stop();
    }
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        pendingProgressCurrent_ = 0;
        pendingProgressTotal_ = 0;
        pendingProgressItemName_.clear();
        hasPendingProgress_ = false;
    }
    if (progressBar_ != nullptr) {
        progressBar_->setRange(0, 100);
        progressBar_->setValue(0);
        progressBar_->setVisible(false);
    }
    if (progressStatusLabel_ != nullptr) {
        progressStatusLabel_->clear();
        progressStatusLabel_->setVisible(false);
    }
    if (btnCancel_ != nullptr) {
        btnCancel_->setVisible(false);
    }
}

void ToolWorkspaceWidget::flushPendingProgress() {
    std::size_t current = 0;
    std::size_t total = 0;
    QString itemName;
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        if (!hasPendingProgress_) {
            return;
        }
        current = pendingProgressCurrent_;
        total = pendingProgressTotal_;
        itemName = pendingProgressItemName_;
        hasPendingProgress_ = false;
    }

    const int maxVal = static_cast<int>(std::max<std::size_t>(1, total));
    const int curVal = static_cast<int>(std::min<std::size_t>(current, static_cast<std::size_t>(maxVal)));
    progressBar_->setRange(0, maxVal);
    progressBar_->setValue(curVal);

    const int availW = (std::max)(80, progressStatusLabel_->width() - 8);
    const QString elided = QFontMetrics(progressStatusLabel_->font())
                               .elidedText(itemName, Qt::ElideMiddle, availW);
    progressStatusLabel_->setText(
        QStringLiteral("%1\n(%2 / %3)").arg(elided).arg(current).arg(total));
}

void ToolWorkspaceWidget::executeCurrentTool() {
    if (isRunning_.exchange(true)) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    const auto opts = gatherOptions();
    persistCurrentOptions();

    if (opts.inputPaths.empty()) {
        isRunning_.store(false);
        emit statusMessage(QStringLiteral("Add at least one input file or folder first."));
        return;
    }

    stopSource_ = std::stop_source{};
    btnExecute_->setEnabled(false);
    btnCancel_->setVisible(true);
    progressBar_->setVisible(true);
    progressBar_->setRange(0, 0);
    progressStatusLabel_->setVisible(true);
    progressStatusLabel_->setText(QStringLiteral("Preparing %1...").arg(toolHeaderBadge_->text()));
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        pendingProgressCurrent_ = 0;
        pendingProgressTotal_ = 0;
        pendingProgressItemName_.clear();
        hasPendingProgress_ = false;
    }
    progressTimer_->start();

    const std::string toolIdStd = currentToolId_.toStdString();
    const QString toolIdCopy = currentToolId_;

    workerThread_ = std::jthread([this, toolIdStd, toolIdCopy, opts](std::stop_token) {
        auto res = mavira::core::archive::ComicMerger::runToolJob(
            toolIdStd,
            opts,
            [this](const mavira::core::archive::MergeProgress& prog) {
                std::lock_guard<std::mutex> lock(progressMutex_);
                pendingProgressCurrent_ = prog.totalPagesProcessed;
                pendingProgressTotal_ = prog.totalPagesEstimated;
                pendingProgressItemName_ = QString::fromStdString(prog.currentVolumeName);
                hasPendingProgress_ = true;
            },
            stopSource_.get_token());

        QString ocrPreview;
        if (res && toolIdStd == "extract_ocr" && !res.value().outputPaths.empty()) {
            std::ifstream ifs(mavira::data::fs::FileManager::toWin32LongPath(res.value().outputPaths.front()),
                              std::ios::binary);
            if (ifs.is_open()) {
                std::ostringstream ss;
                ss << ifs.rdbuf();
                ocrPreview = QString::fromStdString(ss.str());
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, toolIdCopy, res, ocrPreview]() {
                progressTimer_->stop();
                flushPendingProgress();
                isRunning_.store(false);
                btnExecute_->setEnabled(true);
                btnCancel_->setVisible(false);
                progressBar_->setVisible(false);

                if (res) {
                    const auto& r = res.value();
                    QString modeSuffix;
                    if (toolIdCopy != QStringLiteral("extract_ocr")) {
                        modeSuffix = r.usedRawPassThrough
                            ? QStringLiteral(" - 100% Lossless Raw Pass-Through (Container difference only)")
                            : QStringLiteral(" - Re-Encoded Image Stream");
                    }
                    const QString summary = QStringLiteral("Completed: %1 output(s), %2 page(s), %3 skipped%4.")
                                                .arg(r.outputPaths.size())
                                                .arg(r.totalPagesProcessed)
                                                .arg(r.skippedCount)
                                                .arg(modeSuffix);
                    progressStatusLabel_->setText(summary);
                    if (!ocrPreview.isEmpty()) {
                        ocrOutputEdit_->setPlainText(ocrPreview);
                    }
                    const bool mutated = (toolIdCopy == QStringLiteral("merge_comics") ||
                                          toolIdCopy == QStringLiteral("convert_comics") ||
                                          toolIdCopy == QStringLiteral("create_comic"));
                    emit toolCompleted(toolIdCopy, summary, mutated);
                    emit statusMessage(summary);
                } else {
                    const QString errMsg = QStringLiteral("Error: %1")
                                               .arg(QString::fromStdString(res.error().message));
                    progressStatusLabel_->setText(errMsg);
                    emit statusMessage(errMsg);
                }
            },
            Qt::QueuedConnection);
    });
}

void ToolWorkspaceWidget::cancelExecution() {
    stopSource_.request_stop();
    progressStatusLabel_->setText(QStringLiteral("Cancelling..."));
}

void ToolWorkspaceWidget::resetAllToolOptions() {
    mavira::core::ToolExecutionOptions defaults;
    if (settingsManager_ != nullptr) {
        (void)settingsManager_->resetToolOptions(currentToolId_.toStdString());
    }
    applyOptions(defaults);
    emit statusMessage(QStringLiteral("Tool options reset to defaults."));
}

void ToolWorkspaceWidget::resetImageOpsOnly() {
    mavira::core::ToolExecutionOptions def;
    cmbScaleMode_->setCurrentIndex(0);
    spnScale_->setRange(10, 200);
    spnScale_->setValue(def.scaleValue);
    sldScale_->setValue(def.scaleValue);
    cmbTargetImageFormat_->setCurrentIndex(0);
    chkEnableLevels_->setChecked(def.enableLevels);
    spnBlackLevel_->setValue(def.blackLevel);
    spnWhiteLevel_->setValue(def.whiteLevel);
    spnBrightness_->setValue(def.brightness);
    spnSharpness_->setValue(def.sharpness);
    chkGrayscaleManga_->setChecked(def.grayscaleManga);
    spnCropBorderPx_->setValue(def.cropBorderPx);
    chkAutoCropUniform_->setChecked(def.autoCropUniform);
    spnAddBorderPx_->setValue(def.addBorderPx);
    borderColorHex_ = def.borderColorHex;
    btnBorderColor_->setText(QString::fromStdString(borderColorHex_));
    spnWorkerThreads_->setValue(def.workerThreads);
    spnJpgQuality_->setValue(def.jpgQuality);
    spnPngCompression_->setValue(def.pngCompression);
    spnWebpQuality_->setValue(def.webpQuality);
    chkWebpLossless_->setChecked(def.webpLossless);
    chkMergeNormalizeWidth_->setChecked(def.mergeNormalizeWidth);
}

void ToolWorkspaceWidget::onAddFiles() {
    const bool isImgTool = (currentToolId_ == QStringLiteral("convert_images") ||
                            currentToolId_ == QStringLiteral("create_comic"));
    const QString filter = isImgTool
        ? QStringLiteral("Images (*.jpg *.jpeg *.png *.webp *.bmp *.gif);;All Files (*.*)")
        : QStringLiteral("Comics & Documents (*.cbz *.cbr *.cb7 *.cbt *.pdf *.epub *.jpg *.png *.webp);;All Files (*.*)");
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Add Input Files"), {}, filter);
    for (const QString& f : files) {
        inputList_->addItem(QDir::toNativeSeparators(f));
    }
}

void ToolWorkspaceWidget::onAddFolders() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Add Input Folder"));
    if (!dir.isEmpty()) {
        inputList_->addItem(QDir::toNativeSeparators(dir));
    }
}

void ToolWorkspaceWidget::onClearList() {
    inputList_->clear();
}

void ToolWorkspaceWidget::onMoveUp() {
    const int row = inputList_->currentRow();
    if (row > 0) {
        QListWidgetItem* item = inputList_->takeItem(row);
        inputList_->insertItem(row - 1, item);
        inputList_->setCurrentRow(row - 1);
    }
}

void ToolWorkspaceWidget::onMoveDown() {
    const int row = inputList_->currentRow();
    if (row >= 0 && row + 1 < inputList_->count()) {
        QListWidgetItem* item = inputList_->takeItem(row);
        inputList_->insertItem(row + 1, item);
        inputList_->setCurrentRow(row + 1);
    }
}

void ToolWorkspaceWidget::onBrowseOutputDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select Output Folder"), editOutputDir_->text());
    if (!dir.isEmpty()) {
        editOutputDir_->setText(QDir::toNativeSeparators(dir));
        cmbOutputFolderMode_->setCurrentIndex(1);
    }
}

void ToolWorkspaceWidget::onOpenOutputDir() {
    QString dir = editOutputDir_->text().trimmed();
    if (dir.isEmpty() && inputList_->count() > 0) {
        const std::filesystem::path p(inputList_->item(0)->text().toStdWString());
        dir = QString::fromStdWString(p.parent_path().native());
    }
    if (!dir.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
}

void ToolWorkspaceWidget::onPickBorderColor() {
    const QColor initial(QString::fromStdString(borderColorHex_));
    const QColor chosen = QColorDialog::getColor(initial.isValid() ? initial : Qt::black, this);
    if (chosen.isValid()) {
        borderColorHex_ = chosen.name(QColor::HexRgb).toUpper().toStdString();
        btnBorderColor_->setText(QString::fromStdString(borderColorHex_));
    }
}

void ToolWorkspaceWidget::onCopyOcrText() {
    if (QClipboard* cb = QApplication::clipboard(); cb != nullptr) {
        cb->setText(ocrOutputEdit_->toPlainText());
        emit statusMessage(QStringLiteral("OCR text copied to clipboard."));
    }
}

void ToolWorkspaceWidget::onSaveOcrText() {
    const QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save OCR Text"), QStringLiteral("ocr_output.txt"), QStringLiteral("Text Files (*.txt)"));
    if (file.isEmpty()) return;
    const QString nativeFile = QDir::toNativeSeparators(file);
    const std::string utf8 = ocrOutputEdit_->toPlainText().toStdString();
    std::ofstream ofs(mavira::data::fs::FileManager::toWin32LongPath(std::filesystem::path(nativeFile.toStdWString())),
                      std::ios::binary | std::ios::trunc);
    if (ofs.is_open()) {
        ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        emit statusMessage(QStringLiteral("Saved OCR text to %1").arg(file));
    }
}

void ToolWorkspaceWidget::retranslateUi() {
    if (btnCloseTool_ != nullptr) {
        btnCloseTool_->setText(LocalizationManager::tr("tool.close"));
    }
    if (currentToolId_ == QStringLiteral("convert_comics")) {
        if (toolHeaderBadge_) toolHeaderBadge_->setText(LocalizationManager::tr("tool.convert_comics").toUpper());
        if (btnExecute_) btnExecute_->setText(LocalizationManager::tr("tool.btn.convert"));
    } else if (currentToolId_ == QStringLiteral("convert_images")) {
        if (toolHeaderBadge_) toolHeaderBadge_->setText(LocalizationManager::tr("tool.convert_images").toUpper());
        if (btnExecute_) btnExecute_->setText(LocalizationManager::tr("tool.btn.convert"));
    } else if (currentToolId_ == QStringLiteral("create_comic")) {
        if (toolHeaderBadge_) toolHeaderBadge_->setText(LocalizationManager::tr("tool.create_comic").toUpper());
        if (btnExecute_) btnExecute_->setText(LocalizationManager::tr("tool.btn.create"));
    } else if (currentToolId_ == QStringLiteral("extract_pages")) {
        if (toolHeaderBadge_) toolHeaderBadge_->setText(LocalizationManager::tr("tool.extract_pages").toUpper());
        if (btnExecute_) btnExecute_->setText(LocalizationManager::tr("tool.btn.extract"));
    } else if (currentToolId_ == QStringLiteral("extract_ocr")) {
        if (toolHeaderBadge_) toolHeaderBadge_->setText(LocalizationManager::tr("tool.extract_ocr_badge").toUpper());
        if (btnExecute_) btnExecute_->setText(LocalizationManager::tr("tool.btn.run_ocr"));
    } else {
        if (toolHeaderBadge_) toolHeaderBadge_->setText(LocalizationManager::tr("tool.merge_comics").toUpper());
        if (btnExecute_) btnExecute_->setText(LocalizationManager::tr("tool.btn.merge"));
    }
}

} // namespace mavira::ui
