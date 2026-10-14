#pragma once
#include "pdf_optimization.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class PdfOptimizationDialog : public QDialog
{
public:
    explicit PdfOptimizationDialog(PDFDocument document, QWidget* parent = nullptr);
    ~PdfOptimizationDialog() override;
    PDFDocument takeDocument();
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    std::optional<OptimizationResult> candidate;
    std::atomic_bool cancelled = false;
    QThread* job = nullptr;
    QLabel *message, *before, *after;
    QProgressBar* progress;
    QPushButton *apply, *cancel, *retry;
    void start();
};
} // namespace tatsu
