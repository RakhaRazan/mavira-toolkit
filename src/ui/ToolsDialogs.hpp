#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QLabel;

namespace mavira::ui {

/// Dialog for Tools -> Convert -> Comic Books (CBR/CB7/PDF/EPUB -> CBZ/PDF).
class ConvertComicDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConvertComicDialog(QWidget* parent = nullptr);

private slots:
    void browseInput();
    void browseOutput();
    void runConversion();

private:
    QLineEdit* inputEdit_{nullptr};
    QLineEdit* outputEdit_{nullptr};
    QComboBox* formatCombo_{nullptr};
    QSpinBox* qualitySpin_{nullptr};
    QProgressBar* progressBar_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* runBtn_{nullptr};
};

/// Dialog for Tools -> Convert -> Images (JPG/PNG/WEBP).
class BatchConvertImagesDialog : public QDialog {
    Q_OBJECT

public:
    explicit BatchConvertImagesDialog(QWidget* parent = nullptr);

private slots:
    void browseInputs();
    void browseOutputDir();
    void runConversion();

private:
    QLineEdit* inputsEdit_{nullptr};
    QLineEdit* outputDirEdit_{nullptr};
    QComboBox* formatCombo_{nullptr};
    QSpinBox* maxWidthSpin_{nullptr};
    QSpinBox* qualitySpin_{nullptr};
    QProgressBar* progressBar_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* runBtn_{nullptr};
    QStringList selectedFiles_{};
};

/// Dialog for Tools -> Create -> Comic Book from Folder (CBZ/PDF).
class CreateComicDialog : public QDialog {
    Q_OBJECT

public:
    explicit CreateComicDialog(QWidget* parent = nullptr);

private slots:
    void browseFolder();
    void browseOutput();
    void runCreate();

private:
    QLineEdit* folderEdit_{nullptr};
    QLineEdit* outputEdit_{nullptr};
    QComboBox* formatCombo_{nullptr};
    QProgressBar* progressBar_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* runBtn_{nullptr};
};

/// Dialog for Tools -> Extract -> Pages from Comic Book.
class ExtractPagesDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExtractPagesDialog(QWidget* parent = nullptr);

private slots:
    void browseArchive();
    void browseOutputFolder();
    void runExtract();

private:
    QLineEdit* archiveEdit_{nullptr};
    QLineEdit* outputDirEdit_{nullptr};
    QProgressBar* progressBar_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* runBtn_{nullptr};
};

/// Dialog for Tools -> Extract -> Text from Image (Native Windows 10/11 OCR).
class NativeOcrDialog : public QDialog {
    Q_OBJECT

public:
    explicit NativeOcrDialog(QWidget* parent = nullptr);

private slots:
    void browseImage();
    void runOcr();
    void copyText();

private:
    QLineEdit* imageEdit_{nullptr};
    QComboBox* languageCombo_{nullptr};
    QPlainTextEdit* textOutput_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* ocrBtn_{nullptr};
    QPushButton* copyBtn_{nullptr};
};

} // namespace mavira::ui
