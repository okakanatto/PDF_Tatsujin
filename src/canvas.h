#pragma once
#include "document.h"
#include "navigation.h"
#include "search_session.h"
#include "view_state.h"
#include <QtWidgets>
#include <functional>
#include <memory>

namespace tatsu
{
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
    std::function<void(QPointF, QPointF)> draw;
    std::function<void(int)> select;
    std::function<void()> changed;
    std::function<void()> interactionCancelled;
    std::function<void()> escapeReading;
    std::function<void()> viewChanged;
    std::function<void()> toolChanged;
    std::function<void(int)> navigatePage;
    std::function<void(const NavigationTarget&)> navigate;
    explicit Canvas(Document* doc, QWidget* parent = nullptr);
    ~Canvas() override;
    void refresh(PDFObjectReference selection = {});
    void resetView();
    void beginPlacement();
    void beginDrawing(bool line);
    bool drawingActive() const;
    QMap<int, QVector<QRectF>> selectedTextRects() const;
    void finishFormEdit();
    void cancelFormEdit();
    void cancelInteraction();
    void setHandTool(bool enabled);
    bool handToolActive() const;
    void setSearchResults(const QVector<SearchMatch>& matches, quint64 active);
    void showSearchMatch(const SearchMatch& match);
    ViewState viewState() const;
    void restoreView(const ViewState& state);
    bool selectionReady() const;
    QString selectionMessage() const;
    qint64 selectionCacheBytes() const;
    int selectionExtractedPages() const;
    void setZoom(double z);
    void fitWidth();
    void fitPage();
    void goToPage(int number);
    void goToDestination(const NavigationTarget& target);
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
    void keyReleaseEvent(QKeyEvent*) override;

private:
    void updateTool();
    void updatePointerCursor();
    void stopTransientInteraction();
    void requestSelectionText();
    void updateSelection();
    void extendSelection(QPointF point);
    void copySelection();
    void updateAutoScroll();
    struct Impl;
    std::unique_ptr<Impl> d;
    enum class ZoomPolicy
    {
        Manual,
        Relative,
        Automatic
    };
    void updateView(bool force = false);
    void applyZoom(double value, const ViewAnchor& position, ZoomPolicy policy);
    void zoomBy(double factor, const ViewAnchor& position);
    void applyFit(const ViewAnchor& position);
    void mousePress(QMouseEvent*);
    void mouseDoubleClick(QMouseEvent*);
    void mouseMove(QMouseEvent*);
    void mouseRelease(QMouseEvent*);
    const PDFLinkAnnotation* linkAt(int page, QPointF point) const;
    bool signatureAt(int page, QPointF point) const;
};
} // namespace tatsu
