#pragma once
#include "image_pdf.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class ImagePdfDialog : public QDialog
{
public:
    explicit ImagePdfDialog(const QStringList& paths, QWidget* parent = nullptr);
    ~ImagePdfDialog() override;
    void appendFiles(const QStringList& paths);
    PDFDocument takeDocument();
    void reject() override;

protected:
    void accept() override;
    void closeEvent(QCloseEvent* event) override;

private:
    QListWidget* files;
    QLabel *preview, *caption, *message;
    QComboBox* paper;
    QSpinBox* dpi;
    QWidget* settings;
    QPushButton *create, *cancel;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    bool closeAfterCancel = false;
    quint64 generation = 0;
    std::optional<PDFDocument> createdDocument;
    void showPreview();
    void moveSelection(int offset);
    void setWorking(bool working);
    void requestCancel(bool close);
};
} // namespace tatsu
