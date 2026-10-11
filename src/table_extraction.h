#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct TableGrid
{
    QRectF region;
    QVector<double> rows, columns;
};
using TableCells = QVector<QStringList>;
TableCells extractTableCells(PDFDocument document, int page, const TableGrid& grid,
                             const std::function<bool()>& cancelled = {});
void exportTableXlsx(const TableCells& cells, const QString& path,
                     const std::function<bool()>& cancelled = {});
} // namespace tatsu
