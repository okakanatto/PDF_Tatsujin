#include "page_region_preview.h"

namespace tatsu
{
PageRegionPreview::PageRegionPreview(QWidget* parent) : QWidget(parent)
{
    setObjectName("pageRegionPreview");
    setMinimumSize(280, 260);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName("配置範囲のプレビュー。中央をドラッグして移動、右下をドラッグしてサイズ変更"
                      "。数値欄でも変更できます");
}
QRectF PageRegionPreview::paper() const
{
    if (physical.isEmpty())
        return {};
    const double scale =
        qMin((width() - 32.0) / physical.width(), (height() - 32.0) / physical.height());
    const auto size = physical * scale;
    return {(width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
            size.height()};
}
QPointF PageRegionPreview::physicalToWidget(QPointF point) const
{
    const auto box = paper();
    return box.topLeft() + QPointF(point.x() * box.width() / physical.width(),
                                   point.y() * box.height() / physical.height());
}
QPointF PageRegionPreview::widgetToPhysical(QPointF point) const
{
    const auto box = paper();
    return {
        qBound(0.0, (point.x() - box.x()) * physical.width() / box.width(), physical.width()),
        qBound(0.0, (point.y() - box.y()) * physical.height() / box.height(), physical.height())};
}
void PageRegionPreview::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().alternateBase());
    if (image.isNull())
    {
        painter.setPen(palette().text().color());
        painter.drawText(rect(), Qt::AlignCenter, "ページを準備しています…");
        return;
    }
    painter.drawImage(paper(), image);
    painter.setClipRect(paper().adjusted(-5, -5, 5, 5));
    auto paint = [&](QRectF region, bool active)
    {
        const QRectF box(physicalToWidget(region.topLeft()),
                         physicalToWidget(region.bottomRight()));
        painter.setPen(QPen(active ? QColor("#2563eb") : QColor("#64748b"), active ? 2 : 1));
        painter.setBrush(QColor(37, 99, 235, active ? 35 : 12));
        painter.drawRect(box);
        if (active)
        {
            painter.setBrush(QColor("#2563eb"));
            painter.drawRect(QRectF(box.bottomRight() - QPointF(4, 4), QSizeF(8, 8)));
        }
    };
    for (const auto& region : regions)
        paint(gesture && region.first == moving ? pending : region.second,
              region.first == selected);
    if (gesture == 1)
        paint(pending, true);
}
void PageRegionPreview::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || image.isNull() || !paper().contains(event->position()))
        return;
    start = widgetToPhysical(event->position());
    if (drawing)
    {
        gesture = 1;
        moving = -1;
        pending = {start, start};
    }
    else
    {
        int hit = -1;
        for (auto it = regions.crbegin(); it != regions.crend(); ++it)
        {
            const auto corner = physicalToWidget(it->second.bottomRight());
            if (it->second.contains(start) || (corner - event->position()).manhattanLength() <= 12)
            {
                hit = it->first;
                original = it->second;
                gesture = (corner - event->position()).manhattanLength() <= 12 ? 3 : 2;
                break;
            }
        }
        selected = moving = hit;
        if (choose)
            choose(hit);
        if (hit < 0)
            gesture = 0;
        pending = original;
    }
    update();
}
void PageRegionPreview::mouseMoveEvent(QMouseEvent* event)
{
    if (!gesture || image.isNull())
        return;
    const auto point = widgetToPhysical(event->position());
    if (gesture == 1)
        pending = QRectF(start, point).normalized();
    else if (gesture == 2 && original.width() <= physical.width() &&
             original.height() <= physical.height())
        pending = {QPointF(qBound(0.0, original.x() + point.x() - start.x(),
                                  physical.width() - original.width()),
                           qBound(0.0, original.y() + point.y() - start.y(),
                                  physical.height() - original.height())),
                   original.size()};
    else if (gesture == 3 && original.x() >= 0 && original.y() >= 0 &&
             original.x() < physical.width() - minimumSide &&
             original.y() < physical.height() - minimumSide)
        pending = {original.topLeft(),
                   QSizeF(qBound(minimumSide, original.width() + point.x() - start.x(),
                                 physical.width() - original.x()),
                          qBound(minimumSide, original.height() + point.y() - start.y(),
                                 physical.height() - original.y()))};
    update();
}
void PageRegionPreview::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !gesture)
        return;
    mouseMoveEvent(event);
    const int action = gesture;
    gesture = 0;
    if (action == 1 && pending.width() >= minimumSide && pending.height() >= minimumSide && create)
        create(pending);
    else if (action != 1 && move)
        move(moving, pending);
    update();
}
void PageRegionPreview::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && (drawing || gesture))
    {
        gesture = 0;
        drawing = false;
        if (cancelDrawing)
            cancelDrawing();
        setCursor(Qt::ArrowCursor);
        update();
        event->accept();
    }
    else
        QWidget::keyPressEvent(event);
}
} // namespace tatsu
