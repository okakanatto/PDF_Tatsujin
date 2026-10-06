#include "canvas.h"
#include "form_editor.h"
#include "pdf_viewer_adapter.h"
#include "pdfannotation.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentdrawinterface.h"
#include "pdfdrawspacecontroller.h"
#include "pdfdrawwidget.h"
#include "pdffont.h"
#include "pdfpainter.h"
#include "pdfrenderer.h"
#include "selection_text.h"
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
    SelectionTextCache text;
    FormEditor forms;
    QMap<int, QPair<qsizetype, qsizetype>> ranges;
    QTimer autoScroll;
    QTimer preparePages;
    int readingDirection = 1;
    int readingScroll = 0;
    QPointF pointer, selectionStartPoint, selectionEndPoint;
    int selectionStartPage = -1, selectionEndPage = -1;
    qsizetype startCaret = -1;
    qsizetype startWordEnd = -1;
    bool wordSelection = false, pointerInside = false;
    bool suppressWordDoubleClick = false;
    int hoverPage = -1;
    QVector<Signature> hoverSignatures;
    bool selectionComplete = true;
    QString selectionStatus;
    int gesturePage = -1;
    QPointF start, last;
    QTransform gestureMatrix;
    bool dragging = false, selecting = false;
    bool boxPlacement = false, drawing = false, linePlacement = false;
    bool hand = false, temporaryHand = false, panning = false;
    QPointF panPoint;
    const PDFLinkAnnotation* pressedLink = nullptr;
    QPointF pressPosition;

    explicit Impl(Canvas* canvas) : owner(canvas), forms(canvas->document, canvas)
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
        if (drawing && number == gesturePage)
        {
            painter->setPen(QPen(QColor("#1464c0"), 1.5));
            painter->setBrush(QColor(70, 125, 210, 20));
            if (linePlacement)
                painter->drawLine(matrix.map(start), matrix.map(last));
            else
                painter->drawRect(matrix.mapRect(QRectF(start, last).normalized()));
        }
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
        if (const auto range = ranges.constFind(int(number)); range != ranges.cend())
        {
            if (auto contents = text.get(int(number)))
            {
                QRectF line;
                for (qsizetype i = range->first; i < range->second; ++i)
                {
                    if (contents->text[i] == '\n')
                    {
                        painter->fillRect(matrix.mapRect(line), QColor(70, 125, 210, 95));
                        line = {};
                    }
                    else if (contents->boxes[i].isValid())
                        line = line.united(contents->boxes[i]);
                }
                painter->fillRect(matrix.mapRect(line), QColor(70, 125, 210, 95));
            }
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
    d->forms.changed = [this]
    {
        if (changed)
            changed();
    };
    d->forms.draftChanged = [this]
    {
        if (viewChanged)
            viewChanged();
    };
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->view.get());
    setFocusProxy(viewport());
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusPolicy(Qt::StrongFocus);
    viewport()->setAccessibleName("PDF本文。文字選択、Spaceを押しながらドラッグで表示を移動");
    viewport()->installEventFilter(this);
    viewport()->setMouseTracking(true);
    if (window() != this)
        window()->installEventFilter(this);
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
            [this](bool, const std::vector<PDFInteger>&)
            {
                viewport()->update();
                d->preparePages.start();
            });
    d->preparePages.setSingleShot(true);
    d->preparePages.setInterval(30);
    connect(&d->preparePages, &QTimer::timeout, this,
            [this]
            {
                if (!d->snapshot || d->updating || d->resetting || !isVisible())
                    return;
                const auto visible = visiblePages();
                prepareReadingPages(d->proxy, {visible.cbegin(), visible.cend()},
                                    d->readingDirection);
            });
    connect(d->proxy, &PDFDrawWidgetProxy::textLayoutChanged, this,
            [this] { viewport()->update(); });
    connect(&d->text, &SelectionTextCache::pageReady, this,
            [this](int)
            {
                updateSelection();
                updatePointerCursor();
                viewport()->update();
            });
    d->autoScroll.setInterval(16);
    connect(&d->autoScroll, &QTimer::timeout, this,
            [this]
            {
                if (!d->selecting)
                {
                    d->autoScroll.stop();
                    return;
                }
                auto velocity = [](double value, int size)
                {
                    if (value < 32)
                        return -qBound(2, qRound((32 - value) * .7), 36);
                    if (value > size - 32)
                        return qBound(2, qRound((value - size + 32) * .7), 36);
                    return 0;
                };
                const QPoint before(horizontalScrollBar()->value(), verticalScrollBar()->value());
                scrollBy({velocity(d->pointer.x(), viewport()->width()),
                          velocity(d->pointer.y(), viewport()->height())});
                extendSelection(d->pointer);
                if (before == QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value()))
                    d->autoScroll.stop();
            });
}
Canvas::~Canvas()
{
    d->autoScroll.stop();
    d->preparePages.stop();
    d->updating = true;
    viewport()->removeEventFilter(this);
    if (window() != this)
        window()->removeEventFilter(this);
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
    setHandTool(false);
    d->resetting = true;
    d->readingDirection = 1;
    d->readingScroll = 0;
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
    const auto scroll = verticalScrollBar()->value();
    if (scroll != d->readingScroll)
        d->readingDirection = scroll > d->readingScroll ? 1 : -1;
    d->readingScroll = scroll;
    zoom = d->proxy->getZoom();
    auto position = anchor();
    if (position.page >= 0 && position.page != page && !d->dragging)
    {
        d->readingDirection = position.page > page ? 1 : -1;
        page = position.page;
        selected = -1;
        d->items = signatures(document->pdf(), page);
    }
    d->lastAnchor = position;
    d->forms.reposition();
    d->preparePages.start();
    requestSelectionText();
    updatePointerCursor();
    if (viewChanged)
        viewChanged();
}
void Canvas::refresh(PDFObjectReference identity)
{
    d->forms.refresh();
    d->preparePages.stop();
    if (!identity.isValid() && selected >= 0 && selected < d->items.size())
        identity = d->items[selected].ref;
    auto position = d->resetting ? ViewAnchor() : anchor();
    cancelInteraction();
    d->hoverPage = -1;
    d->hoverSignatures.clear();
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
        d->text.setDocument(&document->pdf(), document->revision);
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
    finishFormEdit();
    d->boxPlacement = false;
    setHandTool(false);
    placing = true;
    updateTool();
    setFocus(Qt::OtherFocusReason);
}
void Canvas::beginDrawing(bool line)
{
    beginPlacement();
    d->boxPlacement = true;
    d->linePlacement = line;
}
bool Canvas::drawingActive() const
{
    return d->boxPlacement || d->drawing;
}
QMap<int, QVector<QRectF>> Canvas::selectedTextRects() const
{
    QMap<int, QVector<QRectF>> result;
    if (!document->loaded() || !document->copyAllowed || !d->selectionComplete || copied.isEmpty())
        return result;
    for (auto it = d->ranges.cbegin(); it != d->ranges.cend(); ++it)
    {
        const auto contents = d->text.get(it.key());
        if (!contents)
            return {};
        for (const auto& line : contents->lines)
        {
            QRectF bounds;
            const auto first = qMax(it->first, line.first), end = qMin(it->second, line.end);
            for (auto index = first; index < end; ++index)
                if (contents->boxes[index].isValid())
                    bounds = bounds.united(contents->boxes[index]);
            bounds =
                bounds.intersected(document->pdf().getCatalog()->getPage(it.key())->getCropBox());
            if (bounds.isValid())
                result[it.key()].append(bounds);
        }
    }
    return result;
}
void Canvas::finishFormEdit()
{
    d->forms.finish();
}
void Canvas::cancelFormEdit()
{
    d->forms.cancel();
}
void Canvas::cancelInteraction()
{
    const bool active = placing || d->drawing || d->dragging || d->selecting || d->panning ||
                        d->temporaryHand || d->pressedLink;
    d->boxPlacement = d->drawing = false;
    placing = d->dragging = d->selecting = false;
    d->panning = d->temporaryHand = false;
    d->pressedLink = nullptr;
    copied.clear();
    d->autoScroll.stop();
    d->ranges.clear();
    d->selectionStartPage = d->selectionEndPage = -1;
    d->startCaret = -1;
    d->startWordEnd = -1;
    d->wordSelection = false;
    d->selectionComplete = true;
    d->selectionStatus.clear();
    updateTool();
    viewport()->update();
    if (active && interactionCancelled)
        interactionCancelled();
}

bool Canvas::handToolActive() const
{
    return d->hand || d->temporaryHand;
}
void Canvas::setHandTool(bool enabled)
{
    cancelInteraction();
    d->hand = enabled;
    selected = -1;
    updateTool();
}
void Canvas::updateTool()
{
    const auto cursor = placing || d->drawing ? Qt::CrossCursor
                        : handToolActive()
                            ? (d->panning ? Qt::ClosedHandCursor : Qt::OpenHandCursor)
                            : Qt::ArrowCursor;
    setCursor(cursor);
    viewport()->setCursor(cursor);
    updatePointerCursor();
    if (toolChanged)
        toolChanged();
}
void Canvas::stopTransientInteraction()
{
    if (placing || d->drawing || d->dragging || d->selecting || d->pressedLink)
        cancelInteraction();
    else if (d->panning || d->temporaryHand)
    {
        d->panning = d->temporaryHand = false;
        updateTool();
    }
}
void Canvas::updatePointerCursor()
{
    if (placing || d->drawing || handToolActive() || !document->loaded() || !d->pointerInside ||
        d->revision != document->revision)
        return;
    const int hit = d->hit(d->pointer);
    auto cursor = Qt::ArrowCursor;
    if (hit >= 0)
    {
        const auto point = d->matrix(hit).inverted().map(d->pointer);
        if (signatureAt(hit, point))
            cursor = Qt::SizeAllCursor;
        else if (const auto formCursor = d->forms.cursorAt(hit, point))
            cursor = *formCursor;
        else if (linkAt(hit, point))
            cursor = Qt::PointingHandCursor;
        else if (document->copyAllowed)
            if (const auto text = d->text.get(hit); text && text->contains(point))
                cursor = Qt::IBeamCursor;
    }
    viewport()->setCursor(cursor);
}

bool Canvas::selectionReady() const
{
    return d->selectionComplete;
}
QString Canvas::selectionMessage() const
{
    return d->selectionStatus;
}
qint64 Canvas::selectionCacheBytes() const
{
    return d->text.retainedBytes();
}
int Canvas::selectionExtractedPages() const
{
    return d->text.extractedPages();
}
void Canvas::requestSelectionText()
{
    if (!document->copyAllowed)
        return;
    auto wanted = visiblePages();
    if (d->selectionStartPage >= 0)
    {
        wanted.prepend(d->selectionStartPage);
        wanted.prepend(d->selectionEndPage);
        for (int i = qMin(d->selectionStartPage, d->selectionEndPage);
             i <= qMax(d->selectionStartPage, d->selectionEndPage); ++i)
            wanted.append(i);
    }
    d->text.setWanted(wanted);
}
void Canvas::extendSelection(QPointF point)
{
    const auto snapshot = d->proxy->getSnapshot();
    double nearest = std::numeric_limits<double>::max();
    for (const auto& item : snapshot.items)
    {
        const auto dx =
            std::max({item.rect.left() - point.x(), point.x() - item.rect.right(), 0.0});
        const auto dy =
            std::max({item.rect.top() - point.y(), point.y() - item.rect.bottom(), 0.0});
        const double distance = dx * dx + dy * dy;
        if (distance < nearest)
        {
            nearest = distance;
            d->selectionEndPage = int(item.pageIndex);
            d->selectionEndPoint = item.pageToDeviceMatrix.inverted().map(point);
        }
    }
    requestSelectionText();
    updateSelection();
}
void Canvas::updateSelection()
{
    if (d->selectionStartPage < 0)
        return;
    copied.clear();
    d->ranges.clear();
    d->selectionComplete = true;
    d->selectionStatus.clear();
    auto start = d->text.get(d->selectionStartPage);
    auto end = d->text.get(d->selectionEndPage);
    if (start && d->startCaret < 0)
    {
        if (d->wordSelection)
        {
            const auto span = start->wordAt(d->selectionStartPoint);
            if (span.first < 0)
            {
                cancelInteraction();
                if (!start->hasGlyphs())
                    d->selectionStatus =
                        "このページには文字情報がありません。OCRで選択・コピーできます。";
                if (viewChanged)
                    viewChanged();
                return;
            }
            d->startCaret = span.first;
            d->startWordEnd = span.second;
        }
        else
            d->startCaret = start->caret(d->selectionStartPoint);
    }
    int firstPage = qMin(d->selectionStartPage, d->selectionEndPage);
    int lastPage = qMax(d->selectionStartPage, d->selectionEndPage);
    qsizetype first = 0, last = 0;
    if (start && end)
    {
        first = d->startCaret;
        last = end->caret(d->selectionEndPoint);
        if (d->wordSelection)
        {
            const auto span = end->wordAt(d->selectionEndPoint, true);
            const bool reverse = d->selectionStartPage > d->selectionEndPage ||
                                 (firstPage == lastPage && span.first < first);
            first = reverse ? qMax(qsizetype(0), span.first) : d->startCaret;
            last = reverse ? d->startWordEnd : (span.second >= 0 ? span.second : end->text.size());
        }
        else if (d->selectionStartPage > d->selectionEndPage ||
                 (firstPage == lastPage && first > last))
            std::swap(first, last);
    }
    QStringList pieces;
    for (int number = firstPage; number <= lastPage; ++number)
    {
        if (auto error = d->text.error(number); !error.isEmpty())
        {
            d->selectionComplete = false;
            d->selectionStatus =
                QString("%1ページの文字を解析できません。コピーは実行しません。").arg(number + 1);
        }
        auto contents = d->text.get(number);
        if (!contents || !start || !end)
        {
            d->selectionComplete = false;
            continue;
        }
        const auto from = number == firstPage ? first : 0;
        const auto to = number == lastPage ? last : contents->text.size();
        d->ranges.insert(number, {from, to});
        const auto part = contents->text.mid(from, to - from);
        if (!part.isEmpty())
            pieces.append(part);
    }
    if (!d->selectionComplete)
    {
        if (d->selectionStatus.isEmpty())
            d->selectionStatus = "選択範囲の文字を読み込み中… コピーは完了後に行えます";
    }
    else
    {
        copied = pieces.join("\n\n");
        if (!copied.isEmpty())
            d->selectionStatus = QString("%1文字を選択 · Ctrl+Cでコピー").arg(copied.size());
        else if (start && !start->hasGlyphs())
            d->selectionStatus = "このページには文字情報がありません。OCRで選択・コピーできます。";
    }
    if (viewChanged)
        viewChanged();
    viewport()->update();
}
void Canvas::copySelection()
{
    if (document->copyAllowed && d->selectionComplete && !copied.isEmpty())
        QApplication::clipboard()->setText(copied);
}
void Canvas::updateAutoScroll()
{
    const auto point = d->pointer;
    if (d->selecting && (point.x() < 32 || point.y() < 32 || point.x() > viewport()->width() - 32 ||
                         point.y() > viewport()->height() - 32))
    {
        if (!d->autoScroll.isActive())
            d->autoScroll.start();
    }
    else
        d->autoScroll.stop();
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
    if (d->dragging || d->selecting || d->panning || d->pressedLink)
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
    if (number != page)
        d->readingDirection = number > page ? 1 : -1;
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
    d->pressedLink = nullptr;
    ++d->viewEpoch;
    d->proxy->scrollByPixels(-pixels);
    updateView(true);
}
void Canvas::goToDestination(const NavigationTarget& target)
{
    if (!document->loaded() || !target.valid() || target.page >= document->pages())
        return;
    const auto destination = target.destination;
    const auto previous = anchor({0, 0});
    const auto crop = document->pdf().getCatalog()->getPage(target.page)->getCropBox();
    d->fit = 0;
    goToPage(target.page);
    switch (destination.getDestinationType())
    {
    case DestinationType::Fit:
        fitPage();
        goToPage(target.page);
        break;
    case DestinationType::FitH:
        fitWidth();
        restoreAnchor(
            {target.page,
             {crop.center().x(), destination.hasTop() ? destination.getTop() : previous.point.y()},
             {.5, 0}});
        break;
    case DestinationType::FitV:
        applyZoom(
            d->proxy->getZoomHintForPage(PDFDrawWidgetProxy::ZoomHint::FitHeight, target.page), {});
        restoreAnchor({target.page,
                       {destination.hasLeft() ? destination.getLeft() : previous.point.x(),
                        crop.center().y()},
                       {0, .5}});
        break;
    case DestinationType::FitR:
    {
        const QRectF rectangle(QPointF(destination.getLeft(), destination.getBottom()),
                               QPointF(destination.getRight(), destination.getTop()));
        const auto actual = d->matrix(target.page).mapRect(rectangle);
        if (actual.width() > 0 && actual.height() > 0)
            applyZoom(zoom * qMin(viewport()->width() * .96 / actual.width(),
                                  viewport()->height() * .96 / actual.height()),
                      {target.page, rectangle.center(), {.5, .5}});
        break;
    }
    case DestinationType::XYZ:
    {
        const QPointF point(destination.hasLeft() ? destination.getLeft() : previous.point.x(),
                            destination.hasTop() ? destination.getTop() : previous.point.y());
        applyZoom(destination.hasZoom() && destination.getZoom() > 0 ? destination.getZoom() : zoom,
                  {target.page, point, {0, 0}});
        break;
    }
    default:
        break;
    }
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
    if (d->forms.active())
    {
        try
        {
            finishFormEdit();
        }
        catch (const std::exception& error)
        {
            QMessageBox::warning(this, "フォーム入力を確定できません",
                                 QString::fromUtf8(error.what()));
            return;
        }
    }
    setFocus();
    d->suppressWordDoubleClick = false;
    if (handToolActive())
    {
        d->suppressWordDoubleClick = true;
        d->panning = true;
        d->panPoint = event->position();
        updateTool();
        return;
    }
    const int hit = d->hit(event->position());
    if (hit < 0)
        return;
    const bool placeHere = placing && !document->busy && document->readOnly.isEmpty();
    const bool drawHere = placeHere && d->boxPlacement;
    const bool lineHere = d->linePlacement;
    const auto point = d->matrix(hit).inverted().map(event->position());
    if (!placeHere && document->copyAllowed && d->selectionStartPage >= 0 &&
        event->modifiers().testFlag(Qt::ShiftModifier) && !signatureAt(hit, point) &&
        !linkAt(hit, point))
    {
        d->pressedLink = nullptr;
        d->selecting = true;
        selected = -1;
        d->pointer = event->position();
        extendSelection(d->pointer);
        return;
    }
    cancelInteraction();
    page = d->gesturePage = hit;
    d->items = signatures(document->pdf(), page);
    d->gestureMatrix = d->matrix(page);
    d->start = d->last = d->gestureMatrix.inverted().map(event->position());
    selected = -1;
    if (placeHere)
    {
        d->suppressWordDoubleClick = true;
        if (drawHere)
        {
            d->drawing = true;
            d->linePlacement = lineHere;
            updateTool();
            return;
        }
        if (place)
            place(d->start);
        return;
    }
    if (!document->busy && document->readOnly.isEmpty())
        for (int i = d->items.size() - 1; i >= 0; --i)
            if (d->items[i].kind != OverlayKind::Highlight && d->items[i].rect.contains(d->start))
            {
                selected = i;
                break;
            }
    if (selected >= 0)
    {
        d->suppressWordDoubleClick = true;
        d->dragging = true;
    }
    else
    {
        try
        {
            if (!placeHere && d->forms.press(page, d->start))
            {
                d->suppressWordDoubleClick = true;
                return;
            }
        }
        catch (const std::exception& error)
        {
            QMessageBox::warning(this, "フォーム入力を確定できません",
                                 QString::fromUtf8(error.what()));
            return;
        }
        d->pressedLink = linkAt(page, d->start);
        d->suppressWordDoubleClick = d->pressedLink != nullptr;
        d->pressPosition = event->position();
        if (document->copyAllowed)
        {
            d->selecting = true;
            d->selectionStartPage = d->selectionEndPage = page;
            d->selectionStartPoint = d->selectionEndPoint = d->start;
            d->pointer = event->position();
            requestSelectionText();
            updateSelection();
        }
        else
        {
            d->selectionStatus = "このPDFでは文字のコピーが許可されていません。";
            if (viewChanged)
                viewChanged();
        }
    }
    viewport()->update();
}
void Canvas::mouseDoubleClick(QMouseEvent* event)
{
    // The first click may open a dock or follow a link, moving the PDF beneath
    // the same pointer before Qt sends the second click. Keep that tool's intent.
    if (d->suppressWordDoubleClick || placing || handToolActive() || !document->copyAllowed)
        return;
    const int hit = d->hit(event->position());
    if (hit < 0)
        return;
    const auto point = d->matrix(hit).inverted().map(event->position());
    if (signatureAt(hit, point) || linkAt(hit, point))
        return;
    cancelInteraction();
    selected = -1;
    d->wordSelection = d->selecting = true;
    d->selectionStartPage = d->selectionEndPage = hit;
    d->selectionStartPoint = d->selectionEndPoint = point;
    d->pointer = event->position();
    requestSelectionText();
    updateSelection();
}
void Canvas::mouseMove(QMouseEvent* event)
{
    if (d->pressedLink && (event->position() - d->pressPosition).manhattanLength() >=
                              QApplication::startDragDistance())
        d->pressedLink = nullptr;
    if (d->panning)
    {
        const auto delta = (d->panPoint - event->position()).toPoint();
        // Keep fractional DIP movement across events (e.g. at 150% scaling).
        // Rounding every individual motion would discard slow pointer movement.
        d->panPoint -= QPointF(delta);
        scrollBy(delta);
        return;
    }
    if (d->dragging || d->drawing)
    {
        d->last = d->gestureMatrix.inverted().map(event->position());
        viewport()->update();
    }
    if (d->selecting)
    {
        d->pointer = event->position();
        extendSelection(d->pointer);
        updateAutoScroll();
    }
}
void Canvas::mouseRelease(QMouseEvent* event)
{
    mouseMove(event);
    if (d->drawing)
    {
        const auto start = d->start, finish = d->last;
        const bool valid = d->gestureMatrix == d->matrix(d->gesturePage);
        cancelInteraction();
        if (valid && draw && QLineF(start, finish).length() >= .5)
            draw(start, finish);
        return;
    }
    if (d->pressedLink && d->gestureMatrix == d->matrix(d->gesturePage) &&
        linkAt(d->gesturePage, d->gestureMatrix.inverted().map(event->position())) ==
            d->pressedLink)
    {
        const auto target = resolveLink(document->pdf(), *d->pressedLink);
        cancelInteraction();
        if (navigate)
            navigate(target);
        return;
    }
    d->pressedLink = nullptr;
    if (d->panning)
    {
        d->panning = false;
        updateTool();
        return;
    }
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
        d->autoScroll.stop();
        updateSelection();
    }
    viewport()->update();
}
const PDFLinkAnnotation* Canvas::linkAt(int number, QPointF point) const
{
    if (number < 0 || number >= document->pages() ||
        !document->pdf().getCatalog()->getPage(number)->getCropBox().contains(point))
        return nullptr;
    const auto& annotations = d->annotations->getPageAnnotations(number).annotations;
    for (auto it = annotations.rbegin(); it != annotations.rend(); ++it)
    {
        const auto annotation = it->annotation.data();
        if (!annotation || annotation->getType() != AnnotationType::Link ||
            (annotation->getEffectiveFlags() & (PDFAnnotation::Hidden | PDFAnnotation::NoView)))
            continue;
        const auto link = static_cast<const PDFLinkAnnotation*>(annotation);
        const auto& region = link->getActivationRegion();
        if (region.isEmpty() ? link->getRectangle().contains(point)
                             : region.getPath().contains(point))
            return link;
    }
    return nullptr;
}
bool Canvas::signatureAt(int number, QPointF point) const
{
    if (document->busy || !document->readOnly.isEmpty())
        return false;
    if (d->hoverPage != number)
    {
        d->hoverSignatures = signatures(document->pdf(), number);
        d->hoverPage = number;
    }
    return std::any_of(d->hoverSignatures.cbegin(), d->hoverSignatures.cend(),
                       [point](const Signature& item) {
                           return item.kind != OverlayKind::Highlight && item.rect.contains(point);
                       });
}
bool Canvas::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == viewport() || watched == window()) &&
        (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide ||
         event->type() == QEvent::FocusOut))
        stopTransientInteraction();
    if (watched != viewport())
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::Resize && !d->resizePending)
    {
        d->resizePending = true;
        const auto position = d->lastAnchor;
        const auto epoch = d->viewEpoch;
        stopTransientInteraction();
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
    if (event->type() == QEvent::Leave)
    {
        d->pointerInside = false;
        updateTool();
    }
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick)
    {
        d->pointer = static_cast<QMouseEvent*>(event)->position();
        d->pointerInside = viewport()->rect().contains(d->pointer.toPoint());
        updatePointerCursor();
    }
    if (event->type() == QEvent::ContextMenu)
    {
        auto context = static_cast<QContextMenuEvent*>(event);
        QMenu menu(this);
        menu.setObjectName("textSelectionMenu");
        auto copy = menu.addAction("コピー");
        copy->setObjectName("copySelectedText");
        copy->setShortcut(QKeySequence::Copy);
        copy->setEnabled(document->copyAllowed && d->selectionComplete && !copied.isEmpty());
        connect(copy, &QAction::triggered, this, [this] { copySelection(); });
        menu.exec(context->globalPos());
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress)
    {
        auto mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            mousePress(mouse);
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonDblClick &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
    {
        mouseDoubleClick(static_cast<QMouseEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::MouseMove &&
        (d->drawing || d->dragging || d->selecting || d->panning || d->pressedLink))
    {
        mouseMove(static_cast<QMouseEvent*>(event));
        return true;
    }
    // Canvas owns these tools; the upstream default pan tool resets the cursor
    // to an open hand if it also receives a hover event.
    if (event->type() == QEvent::MouseMove)
        return true;
    if (event->type() == QEvent::ToolTip && !placing && !handToolActive())
    {
        const auto help = static_cast<QHelpEvent*>(event);
        const int hit = d->hit(help->pos());
        if (const auto action =
                hit >= 0 ? linkAt(hit, d->matrix(hit).inverted().map(help->pos())) : nullptr)
        {
            const auto target = resolveLink(document->pdf(), *action);
            QToolTip::showText(help->globalPos(),
                               Qt::convertFromPlainText(
                                   target.valid() ? QString("%1ページへ移動").arg(target.page + 1)
                                                  : target.notice),
                               viewport());
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
    {
        mouseRelease(static_cast<QMouseEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::Wheel)
    {
        d->pressedLink = nullptr;
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
        if (!event->isAccepted())
            QTimer::singleShot(0, this, [this] { updatePointerCursor(); });
        return event->isAccepted();
    }
    if (event->type() == QEvent::KeyRelease)
    {
        keyReleaseEvent(static_cast<QKeyEvent*>(event));
        return event->isAccepted();
    }
    return false;
}
void Canvas::keyPressEvent(QKeyEvent* event)
{
    if (document->loaded() && !document->busy && document->readOnly.isEmpty() &&
        (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab))
    {
        try
        {
            if (d->forms.focusNext(event->key() == Qt::Key_Tab &&
                                   !event->modifiers().testFlag(Qt::ShiftModifier)))
            {
                event->accept();
                return;
            }
        }
        catch (const std::exception& error)
        {
            QMessageBox::warning(this, "フォーム入力を確定できません",
                                 QString::fromUtf8(error.what()));
        }
    }
    if (event->key() == Qt::Key_Delete && selected >= 0 && !document->busy &&
        document->readOnly.isEmpty())
    {
        try
        {
            document->eraseSignature(page, d->items.at(selected));
            selected = -1;
            if (changed)
                changed();
        }
        catch (const std::exception& error)
        {
            QMessageBox::warning(this, "要素を削除できません", QString::fromUtf8(error.what()));
        }
        event->accept();
        return;
    }
    if (document->loaded() && event->key() == Qt::Key_Space &&
        event->modifiers() == Qt::NoModifier && viewport()->hasFocus())
    {
        if (!event->isAutoRepeat() && !d->temporaryHand)
        {
            stopTransientInteraction();
            d->temporaryHand = true;
            updateTool();
        }
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Copy))
    {
        copySelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape)
    {
        const bool active = placing || selected >= 0 || handToolActive() || d->dragging ||
                            d->selecting || d->panning || d->pressedLink ||
                            d->selectionStartPage >= 0;
        setHandTool(false);
        if (!active && escapeReading)
            escapeReading();
        if (viewChanged)
            viewChanged();
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
    if (document->loaded() &&
        (event->matches(QKeySequence::ZoomIn) || event->matches(QKeySequence::ZoomOut) ||
         (event->key() == Qt::Key_Plus &&
          (event->modifiers() & ~Qt::ShiftModifier) == Qt::ControlModifier)))
    {
        const bool out = event->matches(QKeySequence::ZoomOut);
        setZoom(out ? zoom / 1.2 : zoom * 1.2);
        event->accept();
        return;
    }
    if (document->loaded() && event->modifiers() == Qt::ControlModifier)
    {
        if (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)
        {
            if (navigatePage)
                navigatePage(event->key() == Qt::Key_Home ? 0 : document->pages() - 1);
        }
        else if (event->key() == Qt::Key_0)
            fitPage();
        else if (event->key() == Qt::Key_1)
            fitWidth();
        else
        {
            QWidget::keyPressEvent(event);
            return;
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
void Canvas::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && d->temporaryHand)
    {
        if (!event->isAutoRepeat())
        {
            d->temporaryHand = d->panning = false;
            updateTool();
        }
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}
} // namespace tatsu
