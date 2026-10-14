#pragma once
#include "candidate_preview.h"
#include "page_region_preview.h"
#include <QtWidgets>
#include <optional>

namespace tatsu
{
class OfficeImportDialog : public QDialog
{
public:
    explicit OfficeImportDialog(const QString& path, QWidget* parent = nullptr);
    PDFDocument takeDocument();
    void reject() override;

protected:
    void accept() override;
    void closeEvent(QCloseEvent* event) override;

private:
    CandidatePreview worker;
    PageRegionPreview* preview;
    QLabel* message;
    QSpinBox* page;
    QPushButton *apply, *engine;
    QCheckBox* spacing;
    QString source;
    std::optional<PDFDocument> candidate;
    void convert();
};
} // namespace tatsu
