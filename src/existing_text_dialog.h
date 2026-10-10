#pragma once
#include "candidate_preview.h"
#include "existing_text_edit.h"
#include "page_region_preview.h"
#include "text_font_picker.h"
#include <QtWidgets>

namespace tatsu
{
class ExistingTextDialog final : public QDialog
{
public:
    ExistingTextDialog(PDFDocument document, int currentPage, std::function<void()> validate = {},
                       QWidget* parent = nullptr);
    PDFDocument takeDocument() const
    {
        return candidate;
    }
    void accept() override;
    void reject() override;

private:
    PDFDocument snapshot, candidate;
    std::function<void()> validate;
    CandidatePreview worker;
    QVector<ExistingTextBlock> blocks;
    QRectF geometry;
    bool valid = false, closing = false;
    QComboBox *page, *operation;
    QListWidget* list;
    PageRegionPreview* preview;
    QPlainTextEdit* text;
    TextFontPicker* fontChoice;
    QDoubleSpinBox *x, *y, *width, *height;
    QDoubleSpinBox* leading;
    QWidget* leadingSettings;
    QCheckBox* consent;
    QCheckBox* includeGroups;
    QPushButton* apply;
    QLabel *message, *fontInfo;
    QString pageError;
    void loadPage();
    void select(int index);
    void setGeometry(QRectF value);
    void schedule();
    void updateApply();
};
} // namespace tatsu
