#pragma once
#include "candidate_preview.h"
#include "page_region_preview.h"

namespace tatsu
{
class PdfTextDocxDialog : public QDialog
{
public:
    PdfTextDocxDialog(PDFDocument document, int currentPage, std::function<void()> validate,
                      QWidget* parent = nullptr);
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    std::function<void()> validate;
    CandidatePreview worker{this};
    PageRegionPreview* preview;
    QLineEdit *range, *path;
    QComboBox* page;
    QFontComboBox* font;
    QPlainTextEdit* editor;
    QPushButton *extract, *save;
    QLabel* message;
    QVector<int> selected;
    std::shared_ptr<QStringList> prepared;
    QStringList texts;
    QString appliedRange;
    int shown = 0;
    bool edited = false, extracting = false, available = false;
    bool discardEdits();
    void requestTexts();
    void changePage(int index);
    void saveDocument();
};
} // namespace tatsu
