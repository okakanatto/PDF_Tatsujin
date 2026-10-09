#pragma once
#include <QtWidgets>
#include <functional>

namespace tatsu
{
class PageRegionPreview : public QWidget
{
public:
    explicit PageRegionPreview(QWidget* parent = nullptr);
    QImage image;
    QSizeF physical;
    QVector<QPair<int, QRectF>> regions;
    int selected = -1;
    bool drawing = false;
    double minimumSide = 1;
    std::function<void(int)> choose;
    std::function<void(QRectF)> create;
    std::function<void(int, QRectF)> move;
    std::function<void()> cancelDrawing;
    QPointF physicalToWidget(QPointF point) const;
    QRectF paper() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    int gesture = 0;
    int moving = -1;
    QPointF start;
    QRectF original, pending;
    QPointF widgetToPhysical(QPointF point) const;
};
} // namespace tatsu
