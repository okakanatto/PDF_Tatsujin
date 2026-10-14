#pragma once
#include "page_crop.h"
#include <QtWidgets>

namespace tatsu
{
class CropPreview;
class PageCropDialog : public QDialog
{
public:
    PageCropDialog(PDFDocument document, QVector<int> pages, QWidget* parent = nullptr);
    ~PageCropDialog() override;
    QMarginsF margins() const;

private:
    PDFDocument snapshot;
    QVector<int> pages;
    QString restriction;
    QComboBox* page;
    QDoubleSpinBox *top, *right, *bottom, *left;
    CropPreview* preview;
    QLabel *message, *dimensions;
    QPushButton* apply;
    QThread* job = nullptr;
    int shownPage = -1;
    void loadPreview();
    void updateSettings();
};
} // namespace tatsu
