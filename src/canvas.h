#pragma once
#include "document.h"
#include <QtWidgets>
#include <functional>

namespace tatsu
{
class Canvas : public QGraphicsView
{
public:
    Document* document;
    int page = 0;
    double zoom = 1;
    bool placing = false;
    int selected = -1;
    QString copied;
    std::function<void(QPointF)> place;
    std::function<void(int)> select;
    std::function<void()> changed;
    explicit Canvas(Document* doc, QWidget* parent = nullptr);
    void refresh();
    void highlight(const QString& term);
    void setZoom(double z);

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    QGraphicsScene scene;
    QVector<Signature> items;
    QPointF start, last;
    bool dragging = false, selecting = false;
    QGraphicsRectItem* outline = nullptr;
};
} // namespace tatsu
