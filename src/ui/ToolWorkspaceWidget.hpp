#pragma once

#include "core/CommonTypes.hpp"
#include "core/archive/ComicMerger.hpp"
#include "data/config/SettingsManager.hpp"

#include <QString>
#include <QWidget>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;

namespace mavira::ui {

class SectionCardWidget;

/// Full-area modular workspace hosting all 6 MAVIRA utility tools:
/// `convert_comics`, `convert_images`, `create_comic`,
/// `extract_pages`, `extract_ocr`, and `merge_comics`.
class ToolWorkspaceWidget : public QWidget {
    Q_OBJECT

public:
    explicit ToolWorkspaceWidget(
        mavira::data::config::SettingsManager* settingsManager = nullptr,
        QWidget* parent = nullptr);
    ~ToolWorkspaceWidget() override;

    void setSettingsManager(mavira::data::config::SettingsManager* settingsManager) noexcept {
        settingsManager_ = settingsManager;
    }

    /// Opens and configures the workspace for `toolId`, loading persisted options
    /// (or defaults) and optionally pre-populating `initialPaths`.
    void openTool(
        const QString& toolId,
        const std::vector<std::filesystem::path>& initialPaths = {});

    /// Persists current options (when `rememberLastOptions` is enabled) and emits `closeRequested`.
    void closeTool();

    [[nodiscard]] QString currentToolId() const noexcept { return currentToolId_; }
    [[nodiscard]] bool isToolRunning() const noexcept { return isRunning_.load(); }
    [[nodiscard]] mavira::core::ToolExecutionOptions gatherOptions() const;
    void applyOptions(const mavira::core::ToolExecutionOptions& opts);
    void retranslateUi();

    // Accessors for testing & inspection
    [[nodiscard]] QLabel* toolHeaderBadge() const noexcept { return toolHeaderBadge_; }
    [[nodiscard]] QPushButton* primaryActionButton() const noexcept { return btnExecute_; }
    [[nodiscard]] QPushButton* closeToolButton() const noexcept { return btnCloseTool_; }
    [[nodiscard]] QStackedWidget* tabStack() const noexcept { return rightTabStack_; }

public slots:
    void executeCurrentTool();
    void cancelExecution();
    void resetAllToolOptions();
    void resetImageOpsOnly();

signals:
    void closeRequested();
    void toolCompleted(const QString& toolId, const QString& summaryMessage, bool mutatedLibrary);
    void statusMessage(const QString& message);

private:
    void setupUi();
    void configureForTool(const QString& toolId);
    void persistCurrentOptions();

    void onAddFiles();
    void onAddFolders();
    void onClearList();
    void onMoveUp();
    void onMoveDown();
    void onBrowseOutputDir();
    void onOpenOutputDir();
    void onPickBorderColor();
    void onCopyOcrText();
    void onSaveOcrText();

    mavira::data::config::SettingsManager* settingsManager_{nullptr};
    QString currentToolId_{QStringLiteral("convert_comics")};
    std::string borderColorHex_{"#000000"};

    // Left Sidebar (240px)
    QPushButton* btnCloseTool_{nullptr};
    QPushButton* btnExecute_{nullptr};
    QPushButton* btnCancel_{nullptr};
    QProgressBar* progressBar_{nullptr};
    QLabel* progressStatusLabel_{nullptr};
    QButtonGroup* navGroup_{nullptr};
    QPushButton* btnTabGeneral_{nullptr};
    QPushButton* btnTabAdvanced_{nullptr};
    QPushButton* btnTabSettings_{nullptr};

    // Right Content Area
    QLabel* toolHeaderBadge_{nullptr};
    QStackedWidget* rightTabStack_{nullptr};

    // --- Tab 0: General Options ---
    SectionCardWidget* cardInput_{nullptr};
    QListWidget* inputList_{nullptr};
    QPushButton* btnAddFiles_{nullptr};
    QPushButton* btnAddFolders_{nullptr};
    QPushButton* btnClearList_{nullptr};
    QPushButton* btnMoveUp_{nullptr};
    QPushButton* btnMoveDown_{nullptr};

    SectionCardWidget* cardOutput_{nullptr};
    QComboBox* cmbOutputFormat_{nullptr};
    QLabel* lblOutputFormatHint_{nullptr};
    QComboBox* cmbOutputFolderMode_{nullptr};
    QLineEdit* editOutputDir_{nullptr};
    QPushButton* btnChangeOutputDir_{nullptr};
    QPushButton* btnOpenOutputDir_{nullptr};
    QLabel* lblOutputPathHashHint_{nullptr};

    SectionCardWidget* cardImage_{nullptr};
    QSlider* sldScale_{nullptr};
    QSpinBox* spnScale_{nullptr};
    QComboBox* cmbScaleMode_{nullptr};
    QComboBox* cmbTargetImageFormat_{nullptr};

    SectionCardWidget* cardOcrResult_{nullptr};
    QComboBox* cmbOcrLanguage_{nullptr};
    QPlainTextEdit* ocrOutputEdit_{nullptr};
    QPushButton* btnCopyOcr_{nullptr};
    QPushButton* btnSaveOcr_{nullptr};

    // --- Tab 1: Advanced Options ---
    SectionCardWidget* cardAdvInput_{nullptr};
    QFormLayout* advInForm_{nullptr};
    QWidget* extRowContainer_{nullptr};
    QCheckBox* chkExtCbz_{nullptr};
    QCheckBox* chkExtCbr_{nullptr};
    QCheckBox* chkExtCb7_{nullptr};
    QCheckBox* chkExtCbt_{nullptr};
    QCheckBox* chkExtPdf_{nullptr};
    QCheckBox* chkExtEpub_{nullptr};
    QLabel* lblInputRuleNote_{nullptr};
    QComboBox* cmbCreateFolderMode_{nullptr};
    QComboBox* cmbAdvOcrLanguage_{nullptr};
    QLineEdit* editPageRange_{nullptr};
    QDoubleSpinBox* spnOcrUpscale_{nullptr};
    QCheckBox* chkRecursiveFolders_{nullptr};
    QSpinBox* spnPdfExtractDpi_{nullptr};
    QLineEdit* editArchivePassword_{nullptr};

    SectionCardWidget* cardAdvOutput_{nullptr};
    QFormLayout* advOutForm_{nullptr};
    QComboBox* cmbIfExists_{nullptr};
    QComboBox* cmbPageNumberDigits_{nullptr};
    QLabel* lblExtractSubfolderNote_{nullptr};
    QComboBox* cmbOcrOutputFormat_{nullptr};
    QCheckBox* chkOcrIncludeHeaders_{nullptr};
    QCheckBox* chkOcrFilterNoiseSymbols_{nullptr};
    QCheckBox* chkOcrNormalizeAllCaps_{nullptr};
    QCheckBox* chkKeepSubfolders_{nullptr};
    QCheckBox* chkStripMetadata_{nullptr};
    QSpinBox* spnSplitFiles_{nullptr};
    QComboBox* cmbCompressionMode_{nullptr};
    QSpinBox* spnPdfCreationDpi_{nullptr};
    QCheckBox* chkGenerateBookmarks_{nullptr};

    SectionCardWidget* cardAdvImage_{nullptr};
    QCheckBox* chkMergeNormalizeWidth_{nullptr};
    QCheckBox* chkEnableLevels_{nullptr};
    QSpinBox* spnBlackLevel_{nullptr};
    QSpinBox* spnWhiteLevel_{nullptr};
    QLabel* lblBrightness_{nullptr};
    QSpinBox* spnBrightness_{nullptr};
    QLabel* lblSharpness_{nullptr};
    QSpinBox* spnSharpness_{nullptr};
    QCheckBox* chkGrayscaleManga_{nullptr};
    QLabel* lblCropBorder_{nullptr};
    QSpinBox* spnCropBorderPx_{nullptr};
    QCheckBox* chkAutoCropUniform_{nullptr};
    QLabel* lblAddBorder_{nullptr};
    QSpinBox* spnAddBorderPx_{nullptr};
    QPushButton* btnBorderColor_{nullptr};
    QPushButton* btnResetImageOps_{nullptr};
    QSpinBox* spnWorkerThreads_{nullptr};
    QSlider* sldJpgQuality_{nullptr};
    QSpinBox* spnJpgQuality_{nullptr};
    QSlider* sldPngCompression_{nullptr};
    QSpinBox* spnPngCompression_{nullptr};
    QSlider* sldWebpQuality_{nullptr};
    QSpinBox* spnWebpQuality_{nullptr};
    QCheckBox* chkWebpLossless_{nullptr};

    // --- Tab 2: Settings ---
    SectionCardWidget* cardToolSettings_{nullptr};
    QPushButton* btnResetToolOptions_{nullptr};
    QCheckBox* chkRememberLastOptions_{nullptr};

    // Background execution
    void resetProgressState();
    void flushPendingProgress();
    std::stop_source stopSource_{};
    std::jthread workerThread_{};
    std::atomic<bool> isRunning_{false};
    QTimer* progressTimer_{nullptr};
    std::mutex progressMutex_{};
    std::size_t pendingProgressCurrent_{0};
    std::size_t pendingProgressTotal_{0};
    QString pendingProgressItemName_{};
    bool hasPendingProgress_{false};
};

} // namespace mavira::ui
