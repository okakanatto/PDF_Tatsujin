#pragma once
#include "link_edit.h"
#include "link_preview.h"
#include <atomic>
#include <optional>

namespace tatsu
{
class LinkEditDialog : public QDialog
{
public:
    LinkEditDialog(PDFDocument document, int currentPage, QWidget* parent = nullptr);
    ~LinkEditDialog() override;
    PDFDocument takeDocument();
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    QVector<LinkEntry> values;
    QComboBox *page, *target;
    QListWidget* list;
    QLineEdit *description, *url;
    QSpinBox* destination;
    QDoubleSpinBox *x, *y, *width, *height;
    LinkPreview* preview;
    QWidget* settings;
    QLabel* message;
    QPushButton *draw, *remove, *apply, *cancel;
    QThread *renderJob = nullptr, *applyJob = nullptr;
    std::optional<PDFDocument> candidate;
    std::atomic_bool cancelled = false;
    int selected = -1, shownPage = -1;
    bool loading = false;
    void rebuild(int selection = -1);
    void select(int index);
    void updateSettings();
    void updatePreview();
    void loadPreview();
    void add(QRectF rectangle);
};
} // namespace tatsu
