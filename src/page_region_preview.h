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
    bool navigationEnabled = false;
    QColor regionColor = QColor("#2563eb");
    double zoom() const
    {
        return magnification;
    }
    void setZoom(double value, QPointF anchor);
    void fitPage();
    std::function<void(int)> choose;
    std::function<void(QRectF)> create;
    std::function<void(int, QRectF)> move;
    std::function<void()> cancelDrawing;
    std::function<void(double)> zoomChanged;
    QPointF physicalToWidget(QPointF point) const;
    QRectF paper() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    int gesture = 0;
    int moving = -1;
    QPointF start;
    double magnification = 1;
    QPointF pan, panStart;
    QRectF original, pending;
    QPointF widgetToPhysical(QPointF point) const;
    void constrainPan();
};
} // namespace tatsu
