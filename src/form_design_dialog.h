#pragma once
#include "form_design.h"
#include "page_region_preview.h"
#include <atomic>
#include <optional>

namespace tatsu
{
class FormDesignDialog : public QDialog
{
public:
    FormDesignDialog(PDFDocument document, int currentPage, QWidget* parent = nullptr);
    ~FormDesignDialog() override;
    PDFDocument takeDocument();
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    QVector<FormDesignEntry> values;
    QVector<FormField> foreign;
    QVector<QPair<int, int>> positions;
    QComboBox *page, *kind;
    QListWidget* list;
    QLineEdit *name, *caption;
    QPlainTextEdit* initial;
    QTableWidget* options;
    QCheckBox *readOnly, *required, *editableChoice, *multiple;
    QSpinBox* maxLength;
    QDoubleSpinBox *x, *y, *width, *height;
    PageRegionPreview* preview;
    QWidget *settings, *properties;
    QLabel* message;
    QPushButton *draw, *remove, *addOption, *removeOption, *apply, *cancel;
    QTimer* debounce;
    QThread *renderJob = nullptr, *applyJob = nullptr;
    std::optional<PDFDocument> candidate;
    std::atomic_bool cancelled = false;
    quint64 generation = 0;
    int selected = -1;
    bool loading = false;
    void rebuild(int selection = -1);
    void select(int index);
    void updateSettings();
    void updatePreview();
    void schedulePreview();
    void loadPreview();
    void add(QRectF rectangle);
};
} // namespace tatsu
