#pragma once
#include "document_comparison.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class ComparisonDialog : public QDialog
{
public:
    ComparisonDialog(PDFDocument document, QString sourcePath, QByteArray sourceHash,
                     QWidget* parent = nullptr);
    ~ComparisonDialog() override;
    const ComparisonResult* comparison() const;
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot, reference = PDFDocument{};
    QString originalPath, referencePath;
    QByteArray originalHash, referenceHash;
    std::optional<ComparisonResult> result;
    QWidget* settings;
    QLineEdit *path, *password, *output;
    QComboBox *matching, *zoom;
    QTableWidget* table;
    QGraphicsView *leftView, *rightView;
    QLabel *leftLabel, *rightLabel, *message;
    QPlainTextEdit* details;
    QProgressBar* progress;
    QPushButton *start, *save, *cancel, *outputBrowse;
    QThread *job = nullptr, *previewJob = nullptr;
    std::atomic_bool cancelled = false;
    bool closeRequested = false;
    quint64 generation = 0;
    int desiredRow = -1, renderingRow = -1;
    void invalidate();
    void compare();
    void exportResult();
    void showResult();
    void selectRow();
    void loadPreview();
    void requestCancel();
    bool inputsUnchanged() const;
    void busy(bool enabled);
};
} // namespace tatsu
