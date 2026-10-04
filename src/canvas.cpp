#include "canvas.h"
#include "pdfsecurityhandler.h"
#include <QtPrintSupport>

namespace tatsu
{
Canvas::Canvas(Document* d, QWidget* p) : QGraphicsView(p), document(d)
{
    setScene(&scene);
    setBackgroundBrush(QColor("#e5e8ec"));
    setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName("PDFページ。ドラッグで文字選択、署名枠をドラッグで移動");
    setRenderHint(QPainter::Antialiasing);
}
void Canvas::refresh()
{
    const int vertical = verticalScrollBar()->value();
    scene.clear();
    outline = nullptr;
    copied.clear();
    items.clear();
    if (!document->loaded())
        return;
    page = qBound(0, page, document->pages() - 1);
    auto image = renderPage(document->pdf(), page, 1.5);
    auto pix = scene.addPixmap(QPixmap::fromImage(image));
    pix->setScale(1 / 1.5);
    auto dims = pageSize(document->pdf().getCatalog()->getPage(page));
    scene.setSceneRect(QRectF(QPointF(), dims));
    items = signatures(document->pdf(), page);
    if (selected >= items.size())
        selected = -1;
    auto matrix = pageMatrix(document->pdf().getCatalog()->getPage(page));
    for (int i = 0; i < items.size(); ++i)
    {
        auto rect = matrix.mapRect(items[i].rect);
        auto item = scene.addRect(rect, QPen(QColor(i == selected ? "#1464c0" : "#8197ad"),
                                             i == selected ? 1.5 : 0.6, Qt::DashLine));
        item->setZValue(2);
    }
    verticalScrollBar()->setValue(vertical);
}
void Canvas::setZoom(double z)
{
    double top = mapToScene(viewport()->rect().topLeft()).y();
    zoom = qBound(.25, z, 4.0);
    resetTransform();
    scale(zoom, zoom);
    centerOn(scene.sceneRect().center().x(), qMax(0.0, top) + viewport()->height() / (2 * zoom));
}
void Canvas::highlight(const QString& term)
{
    if (!document->loaded() || term.isEmpty())
        return;
    auto layout = textLayout(document->pdf(), page);
    auto matrix = pageMatrix(document->pdf().getCatalog()->getPage(page));
    for (auto& flow : PDFTextFlow::createTextFlows(layout, PDFTextFlow::AddLineBreaks, page))
    {
        auto text = flow.getText();
        auto boxes = flow.getBoundingBoxes();
        int from = 0;
        while ((from = text.indexOf(term, from, Qt::CaseInsensitive)) >= 0)
        {
            for (int i = from; i < from + term.size() && i < int(boxes.size()); ++i)
                scene.addRect(matrix.mapRect(boxes[i]), Qt::NoPen, QColor(255, 195, 0, 90))
                    ->setZValue(1);
            from += term.size();
        }
    }
}
void Canvas::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !document->loaded())
        return QGraphicsView::mousePressEvent(e);
    setFocus();
    start = last = mapToScene(e->position().toPoint());
    auto m = pageMatrix(document->pdf().getCatalog()->getPage(page));
    if (placing && !document->busy)
    {
        placing = false;
        if (place)
            place(m.inverted().map(start));
        return;
    }
    selected = -1;
    if (!document->busy && document->readOnly.isEmpty())
    {
        for (int i = items.size() - 1; i >= 0; --i)
            if (m.mapRect(items[i].rect).contains(start))
            {
                selected = i;
                break;
            }
    }
    if (selected >= 0)
    {
        dragging = true;
        outline = scene.addRect(m.mapRect(items[selected].rect), QPen(QColor("#1464c0"), 2));
        if (select)
            select(selected);
    }
    else
    {
        selecting = true;
        outline = scene.addRect(QRectF(start, start), QPen(QColor("#1464c0"), .5),
                                QColor(50, 120, 230, 35));
    }
}
void Canvas::mouseMoveEvent(QMouseEvent* e)
{
    last = mapToScene(e->position().toPoint());
    if (outline && dragging)
    {
        auto m = pageMatrix(document->pdf().getCatalog()->getPage(page));
        outline->setRect(m.mapRect(items[selected].rect).translated(last - start));
    }
    else if (outline && selecting)
        outline->setRect(QRectF(start, last).normalized());
    else
        QGraphicsView::mouseMoveEvent(e);
}
void Canvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (!document->loaded())
        return;
    last = mapToScene(e->position().toPoint());
    if (dragging)
    {
        dragging = false;
        auto m = pageMatrix(document->pdf().getCatalog()->getPage(page)).inverted();
        try
        {
            if (QLineF(start, last).length() > .1)
                document->moveSignature(page, items[selected], m.map(last) - m.map(start));
        }
        catch (const std::exception& ex)
        {
            QMessageBox::warning(this, "移動できません", QString::fromUtf8(ex.what()));
        }
        if (changed)
            changed();
    }
    if (selecting)
    {
        selecting = false;
        if (document->copyAllowed)
        {
            auto layout = textLayout(document->pdf(), page);
            auto inverse = pageMatrix(document->pdf().getCatalog()->getPage(page)).inverted();
            auto selection =
                layout.createTextSelection(page, inverse.map(start), inverse.map(last));
            copied = layout.getTextFromSelection(selection, page);
        }
        else
            copied.clear();
    }
}
void Canvas::keyPressEvent(QKeyEvent* e)
{
    if (e->matches(QKeySequence::Copy))
    {
        QApplication::clipboard()->setText(copied);
        return;
    }
    if (e->key() == Qt::Key_Escape)
    {
        placing = false;
        setCursor(Qt::ArrowCursor);
        return;
    }
    QGraphicsView::keyPressEvent(e);
}

} // namespace tatsu
