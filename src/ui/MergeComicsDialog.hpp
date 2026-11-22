#pragma once

#include "core/CommonTypes.hpp"
#include "core/archive/ComicMerger.hpp"
#include "data/db/DatabaseManager.hpp"

#include <QDialog>
#include <QString>

#include <atomic>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace mavira::ui {

/// Native dialog for the built-in Batch Comic Merger:
/// - Add & reorder multiple `.cbr`, `.cbz`, `.cb7`, `.cbt`, or `.pdf` inputs (drag-and-drop or buttons)
/// - Choose output format (`.cbz` Zero-Recompression Store, `.cb7` 7-Zip LZMA2, or `.pdf` 1.4 stream)
/// - Save volume start pages as chapter bookmarks
/// - Monitor live 1-page streaming merge progress with cooperative Cancel support
class MergeComicsDialog : public QDialog {
    Q_OBJECT

public:
    explicit MergeComicsDialog(
        mavira::data::db::DatabaseManager* db = nullptr,
        const std::vector<std::filesystem::path>& initialFiles = {},
        QWidget* parent = nullptr);

    ~MergeComicsDialog() override;

    void addInputFiles(const std::vector<std::filesystem::path>& paths);
    void setOutputPath(const std::filesystem::path& path);
    void setOutputFormat(mavira::core::ComicFormat format);

    [[nodiscard]] std::vector<std::filesystem::path> orderedInputFiles() const;
    [[nodiscard]] mavira::core::ComicFormat selectedOutputFormat() const;
    [[nodiscard]] std::filesystem::path outputPath() const;
    [[nodiscard]] bool isMerging() const noexcept {
        return merging_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::optional<mavira::core::archive::MergeResult>& lastResult() const noexcept {
        return lastResult_;
    }

    // Widget accessors for UI integration & testing
    [[nodiscard]] QListWidget* fileList() const noexcept { return fileList_; }
    [[nodiscard]] QComboBox* formatCombo() const noexcept { return cmbFormat_; }
    [[nodiscard]] QLineEdit* outputPathEdit() const noexcept { return editOutputPath_; }
    [[nodiscard]] QProgressBar* progressBar() const noexcept { return progressBar_; }
    [[nodiscard]] QLabel* statusLabel() const noexcept { return lblStatus_; }
    [[nodiscard]] QPushButton* startButton() const noexcept { return btnStart_; }
    [[nodiscard]] QPushButton* cancelButton() const noexcept { return btnCancel_; }

public slots:
    void promptAddFiles();
    void removeSelectedFiles();
    void moveSelectedUp();
    void moveSelectedDown();
    void clearFiles();
    void promptOutputPath();
    void startMerge();
    void cancelMerge();

signals:
    void mergeCompleted(const QString& outputPath, int totalPages, qint64 comicId);
    void mergeFailed(const QString& errorMessage);
    void mergeCancelled();

protected:
    void reject() override;

private slots:
    void onFormatChanged(int index);
    void updateButtonStates();

private:
    void setupUi();
    void setControlsEnabledDuringMerge(bool enabled);
    [[nodiscard]] static QString extensionForFormat(mavira::core::ComicFormat fmt);

    mavira::data::db::DatabaseManager* db_{nullptr};

    QListWidget* fileList_{nullptr};
    QPushButton* btnAddFiles_{nullptr};
    QPushButton* btnRemove_{nullptr};
    QPushButton* btnMoveUp_{nullptr};
    QPushButton* btnMoveDown_{nullptr};
    QPushButton* btnClear_{nullptr};

    QComboBox* cmbFormat_{nullptr};
    QLineEdit* editOutputPath_{nullptr};
    QPushButton* btnBrowseOutput_{nullptr};
    QCheckBox* chkSaveBookmarks_{nullptr};

    QProgressBar* progressBar_{nullptr};
    QLabel* lblStatus_{nullptr};
    QPushButton* btnStart_{nullptr};
    QPushButton* btnCancel_{nullptr};

    std::stop_source mergeStopSource_{};
    std::jthread mergeWorkerThread_{};
    std::atomic<bool> merging_{false};
    std::optional<mavira::core::archive::MergeResult> lastResult_{};
};

} // namespace mavira::ui
