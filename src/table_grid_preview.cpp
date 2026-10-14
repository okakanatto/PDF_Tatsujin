#include "table_grid_preview.h"

namespace tatsu
{
TableGridPreview::TableGridPreview(QWidget* parent) : PageRegionPreview(parent)
{
    setObjectName("tableGridPreview");
    setAccessibleName(
        "表の範囲と境界。範囲を描き、行と列の境界をドラッグします。境界位置欄でも調整できます。");
}
void TableGridPreview::paintEvent(QPaintEvent* event)
{
    PageRegionPreview::paintEvent(event);
    if (!grid.region.isValid() || image.isNull())
        return;
    const auto a = physicalToWidget(grid.region.topLeft()),
               b = physicalToWidget(grid.region.bottomRight());
    const QRectF box(a, b);
    QPainter painter(this);
    painter.setPen(QPen(QColor("#2563eb"), 1.5));
    painter.drawRect(box);
    for (int i = 1; i + 1 < grid.columns.size(); ++i)
    {
        const double x = box.left() + grid.columns[i] * box.width();
        painter.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
    }
    for (int i = 1; i + 1 < grid.rows.size(); ++i)
    {
        const double y = box.top() + grid.rows[i] * box.height();
        painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
    }
}
void TableGridPreview::mousePressEvent(QMouseEvent* event)
{
    if (!drawing && event->button() == Qt::LeftButton && grid.region.isValid())
    {
        const QRectF box(physicalToWidget(grid.region.topLeft()),
                         physicalToWidget(grid.region.bottomRight()));
        if (box.contains(event->position()))
        {
            for (const bool vertical : {true, false})
            {
                const auto& edges = vertical ? grid.columns : grid.rows;
                for (int i = 1; i + 1 < edges.size(); ++i)
                {
                    const auto distance =
                        vertical ? event->position().x() - box.left() - edges[i] * box.width()
                                 : event->position().y() - box.top() - edges[i] * box.height();
                    if (qAbs(distance) <= 6)
                    {
                        boundary = i;
                        column = vertical;
                        before = grid;
                        setCursor(vertical ? Qt::SplitHCursor : Qt::SplitVCursor);
                        return;
                    }
                }
            }
        }
    }
    PageRegionPreview::mousePressEvent(event);
}
void TableGridPreview::mouseMoveEvent(QMouseEvent* event)
{
    if (boundary < 0)
    {
        PageRegionPreview::mouseMoveEvent(event);
        return;
    }
    const QRectF box(physicalToWidget(grid.region.topLeft()),
                     physicalToWidget(grid.region.bottomRight()));
    const double position = column ? (event->position().x() - box.left()) / box.width()
                                   : (event->position().y() - box.top()) / box.height();
    auto& edges = column ? grid.columns : grid.rows;
    edges[boundary] = qBound(edges[boundary - 1] + .002, position, edges[boundary + 1] - .002);
    update();
}
void TableGridPreview::mouseReleaseEvent(QMouseEvent* event)
{
    if (boundary < 0)
    {
        PageRegionPreview::mouseReleaseEvent(event);
        return;
    }
    mouseMoveEvent(event);
    boundary = -1;
    unsetCursor();
    if (changed && !changed(grid))
        grid = before;
    update();
}
} // namespace tatsu
