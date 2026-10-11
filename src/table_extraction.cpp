#include "table_extraction.h"
#include "pdfsecurityhandler.h"
#include <algorithm>
#include <cmath>

namespace tatsu
{
namespace
{
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("表の取り出しを取り消しました。");
}
void validateEdges(const QVector<double>& edges)
{
    if (edges.size() < 2 || edges.size() > 101 || edges.first() != 0 || edges.last() != 1)
        fail("行・列の境界は1〜100区間を指定してください。");
    for (int i = 1; i < edges.size(); ++i)
        if (!std::isfinite(edges[i]) || edges[i] - edges[i - 1] < .001)
            fail("行・列の境界が重なっています。");
}
struct Letter
{
    QChar value;
    QRectF bounds;
    int order;
};
QString cellText(QVector<Letter> letters)
{
    double lineHeight = 0;
    for (const auto& letter : letters)
        lineHeight = qMax(lineHeight, letter.bounds.height());
    std::stable_sort(letters.begin(), letters.end(), [](const auto& a, const auto& b)
                     { return a.bounds.center().y() < b.bounds.center().y(); });
    QStringList lines;
    for (int i = 0; i < letters.size();)
    {
        int end = i + 1;
        const double y = letters[i].bounds.center().y();
        const double tolerance = qMax(.5, lineHeight * .4);
        while (end < letters.size() && letters[end].bounds.center().y() - y <= tolerance)
            ++end;
        std::stable_sort(letters.begin() + i, letters.begin() + end,
                         [](const auto& a, const auto& b)
                         {
                             return a.bounds.left() == b.bounds.left()
                                        ? a.order < b.order
                                        : a.bounds.left() < b.bounds.left();
                         });
        QString line;
        for (int j = i; j < end; ++j)
            line += letters[j].value;
        lines << line.trimmed();
        i = end;
    }
    return lines.join('\n');
}
} // namespace
TableCells extractTableCells(PDFDocument document, int page, const TableGrid& grid,
                             const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (page < 0 || page >= int(document.getCatalog()->getPageCount()))
        fail("表のページがありません。");
    if (!document.getStorage().getSecurityHandler()->isAllowed(
            PDFSecurityHandler::Permission::CopyContent))
        fail("このPDFでは文字のコピーが許可されていません。");
    const auto source = document.getCatalog()->getPage(page);
    if (!grid.region.isValid() || !QRectF(QPointF(), pageSize(source, false)).contains(grid.region))
        fail("表の範囲をページ内に指定してください。");
    validateEdges(grid.rows);
    validateEdges(grid.columns);
    const int rows = grid.rows.size() - 1, columns = grid.columns.size() - 1;
    QVector<QVector<Letter>> assigned(rows * columns);
    const auto matrix = pageMatrix(source, 1, false);
    const auto layout = textLayout(document, page);
    for (const auto& block : layout.getTextBlocks())
        for (const auto& line : block.getLines())
            if (!line.getCharacters().empty() && qAbs(line.getCharacters().front().angle) > .001 &&
                matrix.mapRect(line.getBoundingBox().boundingRect()).intersects(grid.region))
                fail("縦・斜めの文字がある表は、この取り出しでは未対応です。");
    int count = 0, included = 0;
    for (const auto& flow : PDFTextFlow::createTextFlows(layout, PDFTextFlow::AddLineBreaks, page))
    {
        stop(cancelled);
        const auto text = flow.getText();
        const auto boxes = flow.getBoundingBoxes();
        if (text.size() != qsizetype(boxes.size()))
            fail("表の文字の位置を確認できません。");
        for (int i = 0; i < text.size(); ++i)
        {
            if (++count > 200000)
                fail("表のページ文字数が上限を超えています。");
            if (text[i] == '\n' || text[i] == '\r')
                continue;
            auto box = boxes[size_t(i)];
            // Logical spaces inferred by the PDF text layout have no glyph box.
            if (!box.isValid() && text[i].isSpace() && i && i + 1 < text.size() &&
                boxes[size_t(i - 1)].isValid() && boxes[size_t(i + 1)].isValid())
                box = QRectF(QPointF(boxes[size_t(i - 1)].right(), boxes[size_t(i - 1)].top()),
                             QPointF(boxes[size_t(i + 1)].left(), boxes[size_t(i - 1)].bottom()));
            if (!box.isValid())
            {
                if (!text[i].isSpace())
                    fail("表の文字の位置を確認できません。");
                continue;
            }
            box = matrix.mapRect(box);
            const auto intersection = grid.region.intersected(box);
            if (intersection.isEmpty())
                continue;
            if (!grid.region.adjusted(-.1, -.1, .1, .1).contains(box))
            {
                if (text[i].isSpace())
                    continue;
                fail("範囲の端に文字がかかっています。表の範囲を広げてください。");
            }
            const auto center = box.center();
            const auto locate = [](const QVector<double>& edges, double value) {
                return int(std::upper_bound(edges.begin(), edges.end(), value) - edges.begin()) - 1;
            };
            const int row =
                locate(grid.rows, (center.y() - grid.region.top()) / grid.region.height());
            const int column =
                locate(grid.columns, (center.x() - grid.region.left()) / grid.region.width());
            if (row < 0 || row >= rows || column < 0 || column >= columns)
                fail("表の文字をセルへ割り当てられません。");
            const QRectF cell(grid.region.left() + grid.columns[column] * grid.region.width(),
                              grid.region.top() + grid.rows[row] * grid.region.height(),
                              (grid.columns[column + 1] - grid.columns[column]) *
                                  grid.region.width(),
                              (grid.rows[row + 1] - grid.rows[row]) * grid.region.height());
            if (!cell.adjusted(-.1, -.1, .1, .1).contains(box))
            {
                if (text[i].isSpace())
                    continue;
                fail("セル境界に文字がかかっています。境界を移動してください。");
            }
            assigned[row * columns + column] << Letter{text[i], box, count};
            if (!text[i].isSpace())
                ++included;
        }
    }
    if (!included)
        fail("範囲内に文字情報がありません。スキャンPDFは先にOCRしてください。");
    TableCells result;
    for (int row = 0; row < rows; ++row)
    {
        QStringList values;
        for (int column = 0; column < columns; ++column)
            values << cellText(std::move(assigned[row * columns + column]));
        result << values;
    }
    stop(cancelled);
    return result;
}
} // namespace tatsu
