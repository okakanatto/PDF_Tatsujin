#include "page_region_preview.h"
#include <cmath>

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
    const auto size = physical * (scale * magnification);
    return {(width() - size.width()) / 2 + pan.x(), (height() - size.height()) / 2 + pan.y(),
            size.width(), size.height()};
}
void PageRegionPreview::constrainPan()
{
    const auto box = paper();
    const double horizontal = qMax(0.0, (box.width() - width() + 32) / 2);
    const double vertical = qMax(0.0, (box.height() - height() + 32) / 2);
    pan = {qBound(-horizontal, pan.x(), horizontal), qBound(-vertical, pan.y(), vertical)};
}
void PageRegionPreview::setZoom(double value, QPointF anchor)
{
    if (!navigationEnabled || physical.isEmpty())
        return;
    const auto point = widgetToPhysical(anchor);
    magnification = qBound(1.0, value, 8.0);
    pan += anchor - physicalToWidget(point);
    constrainPan();
    gesture = 0;
    update();
}
void PageRegionPreview::fitPage()
{
    magnification = 1;
    pan = {};
    gesture = 0;
    update();
}
void PageRegionPreview::wheelEvent(QWheelEvent* event)
{
    if (!navigationEnabled || image.isNull())
    {
        QWidget::wheelEvent(event);
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier))
        setZoom(magnification * std::pow(1.2, event->angleDelta().y() / 120.0), event->position());
    else if (magnification > 1)
    {
        pan += event->pixelDelta().isNull() ? QPointF(event->angleDelta()) * .5
                                            : QPointF(event->pixelDelta());
        constrainPan();
        update();
    }
    else
    {
        QWidget::wheelEvent(event);
        return;
    }
    event->accept();
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
        painter.setPen(QPen(active ? regionColor : QColor("#64748b"), active ? 2 : 1));
        auto fill = regionColor;
        fill.setAlpha(active ? 35 : 12);
        painter.setBrush(fill);
        painter.drawRect(box);
        if (active)
        {
            painter.setBrush(regionColor);
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
    if (navigationEnabled && event->button() == Qt::MiddleButton && !image.isNull())
    {
        gesture = 4;
        panStart = event->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
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
    if (gesture == 4)
    {
        pan += event->position() - panStart;
        panStart = event->position();
        constrainPan();
        update();
        return;
    }
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
    if (gesture == 4 && event->button() == Qt::MiddleButton)
    {
        mouseMoveEvent(event);
        gesture = 0;
        setCursor(Qt::ArrowCursor);
        update();
        return;
    }
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
