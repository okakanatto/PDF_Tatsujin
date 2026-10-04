#include "canvas.h"
#include "pdfannotation.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentdrawinterface.h"
#include "pdfdrawspacecontroller.h"
#include "pdfdrawwidget.h"
#include "pdffont.h"
#include "pdfpainter.h"
#include "pdfrenderer.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace tatsu
{
struct Canvas::Impl : IDocumentDrawInterface
{
    Canvas* owner;
    PDFCMSManager cms{nullptr};
    PDFFontCache fonts{128, 128};
    std::unique_ptr<PDFDocument> snapshot;
    std::unique_ptr<PDFAnnotationManager> annotations;
    std::unique_ptr<PDFWidget> view;
    PDFDrawWidgetProxy* proxy;
    quint64 revision = std::numeric_limits<quint64>::max();
    bool resetting = true, updating = false, resizePending = false;
    int fit = 1, fitReference = 0;
    ViewAnchor lastAnchor;
    quint64 viewEpoch = 0;
    QVector<Signature> items;
    QVector<SearchMatch> searchRows;
    quint64 activeMatch = 0;
    PDFTextSelection selection;
    std::unique_ptr<PDFTextLayout> selectionLayout;
    int gesturePage = -1;
    QPointF start, last;
    QTransform gestureMatrix;
    bool dragging = false, selecting = false;

    explicit Impl(Canvas* canvas) : owner(canvas)
    {
        view = std::make_unique<PDFWidget>(&cms, RendererEngine::QPainter, canvas);
        proxy = view->getDrawWidgetProxy();
        view->updateCacheLimits(256 * 1024 * 1024, 32 * 1024, 128, 128);
        view->setSmoothWheelScrolling(false);
        annotations = std::make_unique<PDFAnnotationManager>(
            &fonts, &cms, nullptr, PDFMeshQualitySettings(), PDFRenderer::getDefaultFeatures(),
            PDFAnnotationManager::Target::View, nullptr);
        proxy->registerDrawInterface(this);
    }
    ~Impl() override
    {
        // Stop compilers before their immutable snapshot and CMS die.
        view->setDocument(PDFModifiedDocument(), {});
        proxy->unregisterDrawInterface(this);
        view.reset();
    }
    QTransform matrix(int number) const
    {
        auto current = proxy->getSnapshot();
        if (auto item = current.getPageSnapshot(number))
            return item->pageToDeviceMatrix;
        return {};
    }
    int hit(QPointF point) const
    {
        for (const auto& item : proxy->getSnapshot().items)
            if (item.rect.contains(point))
                return int(item.pageIndex);
        return -1;
    }
    void drawPage(QPainter* painter, PDFInteger number, const PDFPrecompiledPage* compiled,
                  PDFTextLayoutGetter& getter, const QTransform& matrix,
                  const PDFColorConvertor& convertor, QList<PDFRenderError>& errors) const override
    {
        annotations->drawPage(painter, number, compiled, getter, matrix, convertor, errors);
        auto it =
            std::lower_bound(searchRows.begin(), searchRows.end(), int(number),
                             [](const SearchMatch& match, int page) { return match.page < page; });
        for (; it != searchRows.end() && it->page == number; ++it)
        {
            const bool active = it->id == activeMatch;
            for (const auto& box : it->boxes)
                painter->fillRect(matrix.mapRect(box),
                                  active ? QColor(255, 162, 0, 125) : QColor(255, 210, 45, 75));
            if (active)
            {
                painter->setPen(QPen(QColor("#996100"), 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(matrix.mapRect(it->bounds).adjusted(-2, -2, 2, 2));
            }
        }
        if (!selection.isEmpty())
        {
            PDFTextSelectionPainter selectionPainter(&selection);
            selectionPainter.draw(painter, number, getter, matrix, convertor);
        }
        if (number == owner->page && owner->selected >= 0 && owner->selected < items.size())
        {
            auto rect = items[owner->selected].rect;
            if (dragging)
                rect.translate(last - start);
            painter->setPen(QPen(QColor("#1464c0"), 1.5, Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(matrix.mapRect(rect));
        }
    }
    void drawPostRendering(QPainter* painter, QRect) const override
    {
        for (const auto& item : proxy->getSnapshot().items)
        {
            if (item.compiledPage && item.compiledPage->isValid())
                continue;
            const bool failed = view->getPageRenderingErrors()->contains(item.pageIndex);
            painter->setPen(failed ? QColor("#9b302a") : QColor("#596779"));
            painter->drawText(item.rect.adjusted(16, 16, -16, -16), Qt::AlignTop | Qt::AlignLeft,
                              QString("%1ページ — %2")
                                  .arg(item.pageIndex + 1)
                                  .arg(failed ? "描画に失敗しました" : "描画中…"));
        }
    }
};

Canvas::Canvas(Document* doc, QWidget* parent)
    : QWidget(parent), document(doc), d(std::make_unique<Impl>(this))
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->view.get());
    setFocusProxy(viewport());
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusPolicy(Qt::StrongFocus);
    viewport()->setAccessibleName("PDF本文。連続スクロール、文字選択、署名の配置と移動");
    viewport()->installEventFilter(this);
    connect(d->proxy, &PDFDrawWidgetProxy::drawSpaceChanged, this, [this] { updateView(); });
    for (auto bar : {verticalScrollBar(), horizontalScrollBar()})
        connect(bar, &QScrollBar::valueChanged, this,
                [this]
                {
                    if (!d->updating && !d->resizePending)
                    {
                        ++d->viewEpoch;
                        updateView(true);
                    }
                });
    connect(d->proxy, &PDFDrawWidgetProxy::pageImageChanged, this,
            [this](bool, const std::vector<PDFInteger>&) { viewport()->update(); });
    connect(d->proxy, &PDFDrawWidgetProxy::textLayoutChanged, this,
            [this] { viewport()->update(); });
}
Canvas::~Canvas()
{
    d->updating = true;
    viewport()->removeEventFilter(this);
    disconnect(d->proxy, nullptr, this, nullptr);
}
QWidget* Canvas::viewport() const
{
    return d->view->getDrawWidget()->getWidget();
}
QScrollBar* Canvas::verticalScrollBar() const
{
    return d->view->getVerticalScrollbar();
}
QScrollBar* Canvas::horizontalScrollBar() const
{
    return d->view->getHorizontalScrollbar();
}
int Canvas::fitMode() const
{
    return d->fit;
}
void Canvas::resetView()
{
    d->resetting = true;
    d->fitReference = page = 0;
}
ViewAnchor Canvas::anchor(QPointF ratio) const
{
    ViewAnchor result;
    result.ratio = ratio;
    const QPointF point(viewport()->width() * ratio.x(), viewport()->height() * ratio.y());
    double distance = std::numeric_limits<double>::max();
    for (const auto& item : d->proxy->getSnapshot().items)
    {
        const auto delta =
            std::max({item.rect.top() - point.y(), point.y() - item.rect.bottom(), 0.0});
        if (delta < distance)
        {
            distance = delta;
            result.page = int(item.pageIndex);
            result.point = item.pageToDeviceMatrix.inverted().map(point);
        }
    }
    return result;
}
void Canvas::restoreAnchor(const ViewAnchor& position)
{
    if (!d->snapshot || position.page < 0 || position.page >= document->pages())
        return;
    ++d->viewEpoch;
    if (!d->proxy->getSnapshot().hasPage(position.page))
        d->proxy->goToPageAndEnsureVisible(position.page, QRectF(position.point, QSizeF(1, 1)));
    if (!d->proxy->getSnapshot().hasPage(position.page))
        return;
    const auto point = pdfToViewport(position.page, position.point);
    const QPointF target(viewport()->width() * position.ratio.x(),
                         viewport()->height() * position.ratio.y());
    d->proxy->scrollByPixels((target - point).toPoint());
    updateView(true);
}
QPointF Canvas::pdfToViewport(int number, QPointF point) const
{
    return d->matrix(number).map(point);
}
QPointF Canvas::viewportToPdf(int number, QPointF point) const
{
    return d->matrix(number).inverted().map(point);
}
QPoint Canvas::mapFromScene(QPointF point) const
{
    return pdfToViewport(
               page, pageMatrix(document->pdf().getCatalog()->getPage(page)).inverted().map(point))
        .toPoint();
}
QPointF Canvas::mapToScene(QPoint point) const
{
    return pageMatrix(document->pdf().getCatalog()->getPage(page)).map(viewportToPdf(page, point));
}
QVector<int> Canvas::visiblePages() const
{
    QVector<int> result;
    for (const auto& item : d->proxy->getSnapshot().items)
        result.append(int(item.pageIndex));
    return result;
}
bool Canvas::pageReady(int number) const
{
    auto snapshot = d->proxy->getSnapshot();
    const auto item = snapshot.getPageSnapshot(number);
    return item && item->compiledPage && item->compiledPage->isValid();
}
void Canvas::updateView(bool force)
{
    if (d->updating || (!force && d->resizePending) || !d->snapshot)
        return;
    zoom = d->proxy->getZoom();
    auto position = anchor();
    if (position.page >= 0 && position.page != page && !d->dragging && !d->selecting)
    {
        page = position.page;
        selected = -1;
        d->items = signatures(document->pdf(), page);
    }
    d->lastAnchor = position;
    if (viewChanged)
        viewChanged();
}
void Canvas::refresh(PDFObjectReference identity)
{
    if (!identity.isValid() && selected >= 0 && selected < d->items.size())
        identity = d->items[selected].ref;
    auto position = d->resetting ? ViewAnchor() : anchor();
    cancelInteraction();
    d->updating = true;
    if (document->loaded() && (d->revision != document->revision || d->resetting))
    {
        // Upstream uses MediaBox and omits UserUnit in its millimetre layout.
        // Adapt only the disposable rendering snapshot; never the saved PDF.
        PDFDocumentBuilder builder(&document->pdf());
        bool cropChanged = false;
        for (int i = 0; i < document->pages(); ++i)
        {
            auto p = document->pdf().getCatalog()->getPage(i);
            if (p->getCropBox() != p->getMediaBox())
            {
                builder.setPageMediaBox(p->getPageReference(), p->getCropBox());
                cropChanged = true;
            }
        }
        auto candidate =
            std::make_unique<PDFDocument>(cropChanged ? builder.build() : document->pdf());
        PDFModifiedDocument modified(candidate.get(), nullptr);
        d->view->setDocument(modified, {});
        d->fonts.setDocument(modified);
        d->annotations->setDocument(modified);
        d->snapshot.swap(candidate);
        PDFDrawSpaceController::LayoutItems layout;
        double y = 0;
        for (int i = 0; i < document->pages(); ++i)
        {
            auto size = pageSize(document->pdf().getCatalog()->getPage(i)) * (25.4 / 72.0);
            layout.emplace_back(0, i, -1,
                                QRectF(-size.width() / 2, y, size.width(), size.height()));
            y += size.height() + 6.35;
        }
        d->proxy->setCustomPageLayout(std::move(layout));
        d->proxy->setPageLayout(PageLayout::Custom);
        d->revision = document->revision;
        d->searchRows.clear();
        d->activeMatch = 0;
    }
    selected = -1;
    d->items.clear();
    if (document->loaded())
    {
        page = qBound(0, page, document->pages() - 1);
        d->items = signatures(document->pdf(), page);
        for (int i = 0; i < d->items.size(); ++i)
            if (d->items[i].ref == identity)
                selected = i;
        if (d->resetting)
        {
            d->proxy->goToPage(page);
            d->resetting = false;
        }
        if (d->fit)
            applyFit(position);
        else
            restoreAnchor(position);
    }
    d->updating = false;
    updateView(true);
    viewport()->update();
}
void Canvas::beginPlacement()
{
    cancelInteraction();
    placing = true;
    setCursor(Qt::CrossCursor);
    viewport()->setCursor(Qt::CrossCursor);
    setFocus(Qt::OtherFocusReason);
}
void Canvas::cancelInteraction()
{
    const bool active = placing || d->dragging || d->selecting;
    placing = d->dragging = d->selecting = false;
    copied.clear();
    d->selection = PDFTextSelection();
    d->selectionLayout.reset();
    setCursor(Qt::ArrowCursor);
    viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();
    if (active && interactionCancelled)
        interactionCancelled();
}
void Canvas::applyZoom(double value, const ViewAnchor& position)
{
    ++d->viewEpoch;
    const bool wasUpdating = d->updating;
    d->updating = true;
    d->proxy->zoom(qBound(.25, value, 4.0));
    restoreAnchor(position);
    d->updating = wasUpdating;
    updateView(true);
}
void Canvas::setZoom(double value)
{
    auto position = anchor();
    if (d->dragging || d->selecting)
        cancelInteraction();
    d->fit = 0;
    applyZoom(value, position);
}
void Canvas::applyFit(const ViewAnchor& position)
{
    const auto hint =
        d->fit == 2 ? PDFDrawWidgetProxy::ZoomHint::Fit : PDFDrawWidgetProxy::ZoomHint::FitWidth;
    applyZoom(d->proxy->getZoomHintForPage(hint, d->fitReference), position);
}
void Canvas::fitWidth()
{
    auto position = anchor();
    cancelInteraction();
    d->fit = 1;
    d->fitReference = page;
    applyFit(position);
}
void Canvas::fitPage()
{
    auto position = anchor();
    cancelInteraction();
    d->fit = 2;
    d->fitReference = page;
    applyFit(position);
}
void Canvas::goToPage(int number)
{
    if (!document->loaded() || number < 0 || number >= document->pages())
        return;
    cancelInteraction();
    ++d->viewEpoch;
    d->updating = true;
    page = number;
    selected = -1;
    d->items = signatures(document->pdf(), page);
    d->fitReference = page;
    if (d->fit)
        applyFit({});
    const auto center = document->pdf().getCatalog()->getPage(page)->getCropBox().center();
    d->proxy->goToPageAndEnsureVisible(page, QRectF(center, QSizeF(1, 1)));
    if (d->proxy->getSnapshot().hasPage(page))
        d->proxy->scrollByPixels(
            QPoint(qRound(viewport()->width() / 2.0 - pdfToViewport(page, center).x()), 0));
    d->proxy->goToPage(page);
    d->updating = false;
    updateView(true);
}
void Canvas::scrollBy(QPoint pixels)
{
    ++d->viewEpoch;
    d->proxy->scrollByPixels(-pixels);
    updateView(true);
}
void Canvas::setSearchResults(const QVector<SearchMatch>& matches, quint64 active)
{
    d->searchRows = matches;
    d->activeMatch = active;
    viewport()->update();
}
ViewState Canvas::viewState() const
{
    return {anchor(), zoom, d->fit, d->fitReference, d->activeMatch};
}
void Canvas::restoreView(const ViewState& state)
{
    if (!document->loaded() || state.anchor.page < 0 || state.anchor.page >= document->pages())
        return;
    cancelInteraction();
    d->fit = state.fitMode;
    d->fitReference = qBound(0, state.fitReference, document->pages() - 1);
    if (d->fit)
        applyFit(state.anchor);
    else
        applyZoom(state.zoom, state.anchor);
}
void Canvas::showSearchMatch(const SearchMatch& match)
{
    if (!document->loaded() || match.page < 0 || match.page >= document->pages())
        return;
    cancelInteraction();
    ++d->viewEpoch;
    d->updating = true;
    d->proxy->goToPageAndEnsureVisible(match.page, match.bounds);
    auto snapshot = d->proxy->getSnapshot();
    if (const auto item = snapshot.getPageSnapshot(match.page))
    {
        const auto box = item->pageToDeviceMatrix.mapRect(match.bounds);
        double dx = 0;
        const double margin = 20;
        if (item->rect.width() <= viewport()->width())
            dx = viewport()->width() / 2.0 - item->rect.center().x();
        else if (box.left() < margin)
            dx = margin - box.left();
        else if (box.right() > viewport()->width() - margin)
            dx = viewport()->width() - margin - box.right();
        d->proxy->scrollByPixels(
            QPoint(qRound(dx), qRound(viewport()->height() * .32 - box.top())));
    }
    page = match.page;
    selected = -1;
    d->items = signatures(document->pdf(), page);
    d->updating = false;
    updateView(true);
    viewport()->update();
}
void Canvas::mousePress(QMouseEvent* event)
{
    const int hit = d->hit(event->position());
    if (hit < 0)
        return;
    setFocus();
    const bool placeHere = placing && !document->busy && document->readOnly.isEmpty();
    cancelInteraction();
    page = d->gesturePage = hit;
    d->items = signatures(document->pdf(), page);
    d->gestureMatrix = d->matrix(page);
    d->start = d->last = d->gestureMatrix.inverted().map(event->position());
    selected = -1;
    if (placeHere)
    {
        if (place)
            place(d->start);
        return;
    }
    if (!document->busy && document->readOnly.isEmpty())
        for (int i = d->items.size() - 1; i >= 0; --i)
            if (d->items[i].rect.contains(d->start))
            {
                selected = i;
                break;
            }
    if (selected >= 0)
        d->dragging = true;
    else if (document->copyAllowed)
    {
        d->selecting = true;
        d->selectionLayout = std::make_unique<PDFTextLayout>(textLayout(document->pdf(), page));
    }
    viewport()->update();
}
void Canvas::mouseMove(QMouseEvent* event)
{
    if (d->dragging || d->selecting)
    {
        d->last = d->gestureMatrix.inverted().map(event->position());
        if (d->selecting)
            d->selection = d->selectionLayout->createTextSelection(d->gesturePage, d->start,
                                                                   d->last, QColor("#6699dd"));
        viewport()->update();
    }
}
void Canvas::mouseRelease(QMouseEvent* event)
{
    mouseMove(event);
    if (d->dragging)
    {
        d->dragging = false;
        try
        {
            if (QLineF(d->start, d->last).length() > .1)
                document->moveSignature(d->gesturePage, d->items[selected], d->last - d->start);
            if (changed)
                changed();
            if (select && selected >= 0)
                select(selected);
        }
        catch (const std::exception& error)
        {
            QMessageBox::warning(this, "移動できません", QString::fromUtf8(error.what()));
        }
    }
    if (d->selecting)
    {
        d->selecting = false;
        copied = document->copyAllowed
                     ? d->selectionLayout->getTextFromSelection(d->selection, d->gesturePage)
                     : QString();
    }
    viewport()->update();
}
bool Canvas::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != viewport())
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::Resize && !d->resizePending)
    {
        d->resizePending = true;
        const auto position = d->lastAnchor;
        const auto epoch = d->viewEpoch;
        if (d->dragging || d->selecting)
            cancelInteraction();
        QTimer::singleShot(0, this,
                           [this, position, epoch]
                           {
                               if (!d->snapshot)
                               {
                                   d->resizePending = false;
                                   return;
                               }
                               const auto target = epoch == d->viewEpoch ? position : d->lastAnchor;
                               if (d->fit)
                                   applyFit(target);
                               else
                               {
                                   restoreAnchor(target);
                               }
                               d->resizePending = false;
                               updateView(true);
                           });
    }
    if (!document->loaded())
        return false;
    if (event->type() == QEvent::MouseButtonPress)
    {
        auto mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            mousePress(mouse);
            return true;
        }
    }
    if (event->type() == QEvent::MouseMove && (d->dragging || d->selecting))
    {
        mouseMove(static_cast<QMouseEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
    {
        mouseRelease(static_cast<QMouseEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::Wheel)
    {
        auto wheel = static_cast<QWheelEvent*>(event);
        if (wheel->modifiers().testFlag(Qt::ControlModifier))
        {
            auto position = anchor({wheel->position().x() / viewport()->width(),
                                    wheel->position().y() / viewport()->height()});
            d->fit = 0;
            cancelInteraction();
            applyZoom(zoom * std::pow(1.2, wheel->angleDelta().y() / 120.0), position);
            return true;
        }
        if (!wheel->pixelDelta().isNull())
        {
            d->proxy->scrollByPixels(wheel->pixelDelta());
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress)
    {
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return event->isAccepted();
    }
    return false;
}
void Canvas::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Copy))
    {
        if (document->copyAllowed)
            QApplication::clipboard()->setText(copied);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape)
    {
        cancelInteraction();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_PageDown || event->key() == Qt::Key_PageUp)
    {
        const int direction = event->key() == Qt::Key_PageDown ? 1 : -1;
        if (event->modifiers().testFlag(Qt::ControlModifier))
        {
            if (navigatePage)
                navigatePage(page + direction);
            else
                goToPage(page + direction);
        }
        else
            scrollBy(QPoint(0, qRound(direction * viewport()->height() * .9)));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
} // namespace tatsu
