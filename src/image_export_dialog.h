#pragma once
#include "image_export.h"
#include <QtWidgets>
#include <atomic>

namespace tatsu
{
class ImageExportDialog : public QDialog
{
public:
    ImageExportDialog(PDFDocument document, int currentPage, const QString& name,
                      QWidget* parent = nullptr);
    ~ImageExportDialog() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    PDFDocument snapshot;
    int current;
    QWidget* settings;
    QLineEdit *directory, *prefix, *range;
    QComboBox *scope, *format;
    QSpinBox* dpi;
    QLabel* message;
    QPlainTextEdit* results;
    QPushButton *run, *cancel;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    void start();
    void setWorking(bool working);
};
} // namespace tatsu
