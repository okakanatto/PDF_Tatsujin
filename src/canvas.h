#pragma once
#include "document.h"
#include <QtWidgets>
#include <functional>
#include <memory>

namespace tatsu
{
struct ViewAnchor
{
    int page = -1;
    QPointF point;
    QPointF ratio{.5, .5};
};
class Canvas : public QWidget
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
    std::function<void()> interactionCancelled;
    std::function<void()> viewChanged;
    explicit Canvas(Document* doc, QWidget* parent = nullptr);
    ~Canvas() override;
    void refresh(PDFObjectReference selection = {});
    void resetView();
    void beginPlacement();
    void cancelInteraction();
    void highlight(const QString& term);
    void setZoom(double z);
    void fitWidth();
    void fitPage();
    void goToPage(int number);
    void scrollBy(QPoint pixels);
    QWidget* viewport() const;
    QScrollBar* verticalScrollBar() const;
    QScrollBar* horizontalScrollBar() const;
    ViewAnchor anchor(QPointF ratio = {.5, .5}) const;
    void restoreAnchor(const ViewAnchor& anchor);
    QPointF pdfToViewport(int number, QPointF point) const;
    QPointF viewportToPdf(int number, QPointF point) const;
    // Page-local, unscaled CropBox coordinates; retained for gesture tests.
    QPoint mapFromScene(QPointF point) const;
    QPointF mapToScene(QPoint point) const;
    QVector<int> visiblePages() const;
    bool pageReady(int number) const;
    int fitMode() const;

protected:
    bool eventFilter(QObject*, QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    void updateView(bool force = false);
    void applyZoom(double value, const ViewAnchor& position);
    void applyFit(const ViewAnchor& position);
    void mousePress(QMouseEvent*);
    void mouseMove(QMouseEvent*);
    void mouseRelease(QMouseEvent*);
};
} // namespace tatsu
