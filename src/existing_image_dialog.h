#pragma once
#include "existing_image_edit.h"
#include "page_region_preview.h"
#include <atomic>

namespace tatsu
{
class ExistingImageDialog : public QDialog
{
public:
    ExistingImageDialog(PDFDocument document, int currentPage, std::function<void()> validate,
                        QWidget* parent = nullptr);
    ~ExistingImageDialog() override;
    PDFDocument takeDocument()
    {
        return std::move(candidate);
    }
    void accept() override;
    void reject() override;

private:
    PDFDocument snapshot, candidate;
    std::function<void()> validate;
    QVector<ExistingImage> images;
    QRectF geometry;
    QImage replacement;
    QByteArray replacementHash;
    QComboBox *page, *operation;
    QListWidget* list;
    PageRegionPreview* preview;
    QDoubleSpinBox *x, *y, *width, *height;
    QCheckBox *aspect, *confirmDelete, *includeGroups;
    QLineEdit* replacementPath;
    QLabel* message;
    QString pageError;
    QPushButton *apply, *cancel, *pick;
    QThread* job = nullptr;
    QTimer timer;
    quint64 generation = 0;
    bool closing = false, validPreview = false;
    std::shared_ptr<std::atomic_bool> stopped = std::make_shared<std::atomic_bool>(false);
    void loadPage();
    void selectImage(int index);
    void setGeometry(QRectF rectangle);
    void geometryChanged(QDoubleSpinBox* sender);
    void loadReplacement();
    void schedule();
    void render();
    void updateApply();
};
} // namespace tatsu
