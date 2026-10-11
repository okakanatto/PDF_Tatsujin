#include "table_extraction_dialog.h"
#include "pdfdocumentbuilder.h"

namespace tatsu
{
namespace
{
QVector<double> equalEdges(int count)
{
    QVector<double> edges;
    for (int i = 0; i <= count; ++i)
        edges << double(i) / count;
    return edges;
}
} // namespace
TableExtractionDialog::TableExtractionDialog(PDFDocument document, int currentPage,
                                             std::function<void()> validate, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), validate(std::move(validate))
{
    setObjectName("tableExtractionDialog");
    setWindowTitle("PDFの表をExcelへ取り出す");
    resize(1160, 730);
    setMinimumSize(820, 520);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel(
        "表の範囲を描き、境界を調整してセルを確認します。ページの回転を外した抽出用表示です。"
        "原PDFは変更しません。本文の文字を扱い、注釈・フォームは含めません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto controls = new QHBoxLayout;
    page = new QComboBox;
    page->setObjectName("tablePage");
    page->setAccessibleName("表を取り出すページ");
    for (int i = 0; i < int(snapshot.getCatalog()->getPageCount()); ++i)
        page->addItem(QString("%1ページ").arg(i + 1));
    current = qBound(0, currentPage, page->count() - 1);
    page->setCurrentIndex(current);
    rows = new QSpinBox;
    columns = new QSpinBox;
    rows->setObjectName("tableRows");
    columns->setObjectName("tableColumns");
    for (auto spin : {rows, columns})
    {
        spin->setRange(1, 100);
        spin->setKeyboardTracking(false);
    }
    rows->setValue(4);
    columns->setValue(3);
    grid.rows = equalEdges(4);
    grid.columns = equalEdges(3);
    draw = new QPushButton("範囲を描く");
    draw->setObjectName("tableDraw");
    draw->setCheckable(true);
    draw->setChecked(true);
    controls->addWidget(page);
    controls->addWidget(new QLabel("行"));
    controls->addWidget(rows);
    controls->addWidget(new QLabel("列"));
    controls->addWidget(columns);
    controls->addWidget(draw);
    boundary = new QComboBox;
    boundary->setObjectName("tableBoundary");
    position = new QDoubleSpinBox;
    position->setObjectName("tableBoundaryPosition");
    position->setDecimals(2);
    position->setKeyboardTracking(false);
    position->setSuffix(" %");
    controls->addWidget(new QLabel("境界"));
    controls->addWidget(boundary);
    controls->addWidget(position);
    layout->addLayout(controls);
    auto split = new QSplitter;
    preview = new TableGridPreview;
    preview->navigationEnabled = true;
    preview->drawing = true;
    cells = new QTableWidget;
    cells->setObjectName("tableCells");
    cells->setAccessibleName("取り出した表。セルを編集してから保存できます");
    cells->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    cells->setWordWrap(true);
    split->addWidget(preview);
    split->addWidget(cells);
    split->setSizes({580, 540});
    layout->addWidget(split, 1);
    message = new QLabel("ページを表示しています…");
    message->setObjectName("tableMessage");
    message->setWordWrap(true);
    layout->addWidget(message);
    auto output = new QHBoxLayout;
    path = new QLineEdit;
    path->setObjectName("tableOutputPath");
    path->setPlaceholderText("新しいExcelファイルの保存先");
    auto choose = new QPushButton("保存先…");
    output->addWidget(path, 1);
    output->addWidget(choose);
    save = new QPushButton("Excelへ保存");
    save->setObjectName("tableSave");
    save->setEnabled(false);
    output->addWidget(save);
    auto cancel = new QPushButton("閉じる");
    cancel->setObjectName("tableCancel");
    output->addWidget(cancel);
    layout->addLayout(output);
    for (auto button : {draw, choose, save, cancel})
        button->setAutoDefault(false);
    connect(choose, &QPushButton::clicked, this,
            [this]
            {
                const auto selected = QFileDialog::getSaveFileName(
                    this, "新しいExcelファイルへ保存", path->text(), "Excelブック (*.xlsx)",
                    nullptr, QFileDialog::DontConfirmOverwrite);
                if (!selected.isEmpty())
                    path->setText(selected);
            });
    connect(save, &QPushButton::clicked, this, [this] { saveWorkbook(); });
    connect(cancel, &QPushButton::clicked, this, [this] { reject(); });
    connect(draw, &QPushButton::toggled, this,
            [this](bool checked) { preview->drawing = checked; });
    preview->create = [this](QRectF region)
    {
        auto proposal = grid;
        proposal.region = region;
        if (changeGrid(std::move(proposal)))
            draw->setChecked(false);
    };
    preview->changed = [this](const TableGrid& proposal) { return changeGrid(proposal); };
    preview->cancelDrawing = [this] { draw->setChecked(false); };
    connect(rows, &QSpinBox::valueChanged, this,
            [this](int count)
            {
                auto proposal = grid;
                proposal.rows = equalEdges(count);
                if (!changeGrid(std::move(proposal)))
                {
                    QSignalBlocker blocker(rows);
                    rows->setValue(grid.rows.size() - 1);
                }
            });
    connect(columns, &QSpinBox::valueChanged, this,
            [this](int count)
            {
                auto proposal = grid;
                proposal.columns = equalEdges(count);
                if (!changeGrid(std::move(proposal)))
                {
                    QSignalBlocker blocker(columns);
                    columns->setValue(grid.columns.size() - 1);
                }
            });
    connect(page, &QComboBox::currentIndexChanged, this,
            [this](int number)
            {
                if (!discardEdits())
                {
                    QSignalBlocker blocker(page);
                    page->setCurrentIndex(current);
                    return;
                }
                current = number;
                grid.region = {};
                preview->image = {};
                preview->grid = grid;
                draw->setChecked(true);
                request();
            });
    connect(cells, &QTableWidget::itemChanged, this, [this] { edited = true; });
    connect(boundary, &QComboBox::currentIndexChanged, this,
            [this]
            {
                const auto encoded = boundary->currentData().toInt();
                const auto& edges = encoded > 0 ? grid.columns : grid.rows;
                const int index = qAbs(encoded);
                QSignalBlocker blocker(position);
                position->setEnabled(index > 0 && index + 1 < edges.size());
                if (position->isEnabled())
                {
                    position->setRange(edges[index - 1] * 100 + .2, edges[index + 1] * 100 - .2);
                    position->setValue(edges[index] * 100);
                }
            });
    connect(position, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                const auto encoded = boundary->currentData().toInt();
                if (!encoded)
                    return;
                auto proposal = grid;
                (encoded > 0 ? proposal.columns : proposal.rows)[qAbs(encoded)] = value / 100;
                changeGrid(std::move(proposal));
            });
    worker.ready = [this](CandidatePreviewResult result)
    {
        if (!result.error.isEmpty())
        {
            message->setText(result.error);
            return;
        }
        preview->image = result.image;
        preview->physical = result.dimensions;
        preview->grid = grid;
        preview->update();
        cells->setEnabled(true);
        QSignalBlocker blocker(cells);
        cells->clear();
        cells->setRowCount(prepared->size());
        cells->setColumnCount(prepared->isEmpty() ? 0 : prepared->first().size());
        for (int row = 0; row < prepared->size(); ++row)
            for (int column = 0; column < (*prepared)[row].size(); ++column)
                cells->setItem(row, column, new QTableWidgetItem((*prepared)[row][column]));
        cells->resizeRowsToContents();
        edited = false;
        available = !prepared->isEmpty();
        save->setEnabled(available);
        message->setText(available ? "セルを確認・修正して、新しいExcelへ保存できます。全セルを文字"
                                     "列として保存します。"
                                   : "プレビューで表の範囲を描いてください。");
    };
    boundaries();
    request();
}
bool TableExtractionDialog::discardEdits()
{
    if (edited && QMessageBox::question(this, "セルの編集を保持",
                                        "再抽出するとセルの編集が置き換わります。続けますか。",
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::No) != QMessageBox::Yes)
        return false;
    edited = false;
    return true;
}
bool TableExtractionDialog::changeGrid(TableGrid proposal)
{
    if (!discardEdits())
        return false;
    grid = std::move(proposal);
    preview->grid = grid;
    preview->update();
    boundaries();
    request();
    return true;
}
void TableExtractionDialog::boundaries()
{
    const auto previous = boundary->currentData().toInt();
    QSignalBlocker blocker(boundary);
    boundary->clear();
    for (int i = 1; i + 1 < grid.columns.size(); ++i)
        boundary->addItem(QString("列 %1").arg(i), i);
    for (int i = 1; i + 1 < grid.rows.size(); ++i)
        boundary->addItem(QString("行 %1").arg(i), -i);
    boundary->setCurrentIndex(qMax(0, boundary->findData(previous)));
    const int encoded = boundary->currentData().toInt(), index = qAbs(encoded);
    const auto& edges = encoded > 0 ? grid.columns : grid.rows;
    QSignalBlocker valueBlocker(position);
    position->setEnabled(index > 0 && index + 1 < edges.size());
    if (position->isEnabled())
    {
        position->setRange(edges[index - 1] * 100 + .2, edges[index + 1] * 100 - .2);
        position->setValue(edges[index] * 100);
    }
}
void TableExtractionDialog::request()
{
    available = false;
    save->setEnabled(false);
    cells->setEnabled(false);
    message->setText("ページと文字を確認しています…");
    prepared = std::make_shared<TableCells>();
    const auto values = prepared;
    const auto source = snapshot;
    const auto selected = grid;
    const auto number = current;
    worker.request(
        [source, selected, number, values](const auto& cancelled)
        {
            PDFDocumentBuilder builder(&source);
            builder.setPageRotation(source.getCatalog()->getPage(number)->getPageReference(),
                                    PageRotation::None);
            auto upright = builder.build();
            if (selected.region.isValid())
                *values = extractTableCells(upright, number, selected, cancelled);
            return upright;
        },
        number, 1200);
}
void TableExtractionDialog::saveWorkbook()
{
    if (!available)
        return;
    try
    {
        if (validate)
            validate();
        TableCells values;
        for (int row = 0; row < cells->rowCount(); ++row)
        {
            QStringList line;
            for (int column = 0; column < cells->columnCount(); ++column)
                line << cells->item(row, column)->text();
            values << line;
        }
        exportTableXlsx(values, path->text());
        edited = false;
        message->setText("保存しました：" + QFileInfo(path->text()).absoluteFilePath());
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
    }
}
void TableExtractionDialog::reject()
{
    if (edited && QMessageBox::question(
                      this, "セル編集を保存", "編集したセルは保存されていません。閉じますか。",
                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    save->setEnabled(false);
    worker.cancel([this] { QDialog::reject(); });
}
void TableExtractionDialog::closeEvent(QCloseEvent* event)
{
    event->ignore();
    reject();
}
} // namespace tatsu
