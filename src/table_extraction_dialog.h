#pragma once
#include "candidate_preview.h"
#include "table_grid_preview.h"

namespace tatsu
{
class TableExtractionDialog : public QDialog
{
public:
    TableExtractionDialog(PDFDocument document, int currentPage, std::function<void()> validate,
                          QWidget* parent = nullptr);
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    std::function<void()> validate;
    CandidatePreview worker{this};
    TableGridPreview* preview;
    TableGrid grid;
    QComboBox *page, *boundary;
    QSpinBox *rows, *columns;
    QDoubleSpinBox* position;
    QTableWidget* cells;
    QLineEdit* path;
    QPushButton *save, *draw;
    QLabel* message;
    int current = 0;
    bool edited = false, available = false;
    std::shared_ptr<TableCells> prepared;
    bool discardEdits();
    bool changeGrid(TableGrid proposal);
    void request();
    void boundaries();
    void saveWorkbook();
};
} // namespace tatsu
