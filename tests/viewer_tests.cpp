#include "viewer_tests.h"
#include "page_previews.h"
#include "window.h"
#include <QtTest/QTest>
#include <algorithm>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
void ready(Canvas* canvas, int page, const QString& stage = {})
{
    check(QTest::qWaitFor([&] { return canvas->pageReady(page); }, 10000),
          QString("page %1 compiled and visible (%2, current=%3, scroll=%4)")
              .arg(page + 1)
              .arg(stage)
              .arg(canvas->page + 1)
              .arg(canvas->verticalScrollBar()->value()));
}
double anchorError(Canvas* canvas, const ViewAnchor& anchor)
{
    auto actual = canvas->pdfToViewport(anchor.page, anchor.point);
    auto expected = QPointF(canvas->viewport()->width() * anchor.ratio.x(),
                            canvas->viewport()->height() * anchor.ratio.y());
    return qMax(qAbs(actual.x() - expected.x()), qAbs(actual.y() - expected.y()));
}
} // namespace

QJsonObject testPagePreviews(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D10-digital-100.pdf");
    auto panel = qobject_cast<PagePreviews*>(window.pages);
    check(panel != nullptr, "page list owns asynchronous previews");
    int ticks = 0;
    QTimer pulse;
    QObject::connect(&pulse, &QTimer::timeout, [&] { ++ticks; });
    pulse.start(1);
    check(QTest::qWaitFor(
              [&] { return !panel->preview(0).isNull() && !panel->preview(2).isNull(); }, 15000),
          "visible page previews finish");
    pulse.stop();
    const int atOpen = panel->renderedPages();
    check(atOpen >= 3 && atOpen < 10 && ticks > 0,
          "100-page open only renders visible previews while the UI responds");
    const auto original = encodePdf(window.doc.pdf());
    const auto before = window.canvas->viewState();
    panel->scrollToBottom();
    check(QTest::qWaitFor([&] { return !panel->preview(99).isNull(); }, 15000),
          "scrolling page list loads the last preview");
    check(window.canvas->page == before.anchor.page && !window.doc.dirty() &&
              window.doc.cursor == 0,
          "preview loading and list scrolling do not move the document or edit it");
    const auto target = panel->item(99);
    const auto position = panel->visualItemRect(target).center();
    check(panel->viewport()->rect().contains(position), "last preview row is visible");
    QTest::mouseClick(panel->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    ready(window.canvas, 99);
    check(window.canvas->page == 99, "preview click navigates to the exact page");
    window.findChild<QAction*>("previousView")->trigger();
    ready(window.canvas, before.anchor.page);
    check(anchorError(window.canvas, before.anchor) <= 2, "preview navigation has reading history");
    check(encodePdf(window.doc.pdf()) == original, "previews do not modify the PDF");
    for (int page = 0; page < 100; ++page)
    {
        panel->scrollToItem(panel->item(page));
        check(QTest::qWaitFor([&] { return !panel->preview(page).isNull(); }, 15000),
              "all visited previews finish without whole-document preloading");
        check(panel->cacheBytes() <= 8 * 1024 * 1024, "preview LRU retains at most 8 MiB");
    }
    const auto bytes = panel->cacheBytes();
    check(bytes <= 8 * 1024 * 1024, "preview cache stays within measured 8 MiB target");
    window.grab().save(output + "/page-previews-100.png");
    window.openFile(fixtures + "/D02.pdf");
    check(QTest::qWaitFor(
              [&] { return !panel->preview(0).isNull() && !panel->preview(2).isNull(); }, 15000),
          "replacement document previews finish");
    QJsonArray dimensions;
    for (int page = 0; page < window.doc.pages(); ++page)
    {
        panel->scrollToItem(panel->item(page));
        check(QTest::qWaitFor([&] { return !panel->preview(page).isNull(); }, 15000),
              "rotated and clipped preview ready");
        const auto image = panel->preview(page);
        const auto size = pageSize(window.doc.pdf().getCatalog()->getPage(page));
        auto expected = size;
        expected.scale(QSizeF(96, 128) * image.devicePixelRatio(), Qt::KeepAspectRatio);
        check(qAbs(image.width() - expected.width()) <= 1 &&
                  qAbs(image.height() - expected.height()) <= 1,
              "preview preserves rotated CropBox and UserUnit aspect ratio");
        auto reference =
            renderPage(window.doc.pdf(), page,
                       qMin(96.0 / size.width(), 128.0 / size.height()) * image.devicePixelRatio());
        reference.setDevicePixelRatio(image.devicePixelRatio());
        check(image == reference, "cached preview belongs to current document and geometry");
        dimensions.append(QJsonObject{{"page", page + 1},
                                      {"width", image.width()},
                                      {"height", image.height()},
                                      {"DPR", image.devicePixelRatio()}});
    }
    window.grab().save(output + "/page-previews-rotation.png");
    window.openFile(fixtures + "/D01.pdf");
    check(QTest::qWaitFor([&] { return !panel->preview(0).isNull(); }, 15000), "D01 preview ready");
    const auto baseline = panel->preview(0);
    window.doc.putSignature(0, "山田 太郎", {80, 400}, 40, Qt::black);
    window.refresh();
    check(panel->preview(0).isNull(), "revision removes stale preview immediately");
    check(QTest::qWaitFor([&] { return !panel->preview(0).isNull(); }, 15000),
          "signature preview ready");
    check(panel->preview(0) != baseline, "new signature appears in thumbnail");
    window.undoAction->trigger();
    check(QTest::qWaitFor([&] { return !panel->preview(0).isNull(); }, 15000),
          "Undo preview ready");
    check(panel->preview(0) == baseline, "Undo restores exact baseline thumbnail");
    window.openFile(fixtures + "/D10-image-50.pdf");
    QCoreApplication::processEvents();
    window.openFile(fixtures + "/D01.pdf");
    check(QTest::qWaitFor([&] { return !panel->preview(0).isNull(); }, 15000),
          "preview generation can change during background work");
    check(panel->preview(0) == baseline, "late scan preview cannot replace the new document");
    auto tabs = window.findChild<QTabWidget*>("navigationTabs");
    tabs->setCurrentIndex(1);
    QTest::qWait(100);
    const int hiddenStart = panel->renderedPages();
    QTest::qWait(100);
    check(panel->renderedPages() == hiddenStart, "hidden page panel starts no further rendering");
    return {{"pages_rendered_at_open", atOpen},
            {"UI_timer_ticks", ticks},
            {"cache_bytes_after_navigation", bytes},
            {"geometry", dimensions},
            {"signature_Undo", true},
            {"late_generation_rejected", true},
            {"navigation_and_PDF_unchanged", true}};
}

QJsonObject testViewerNavigation(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D10-digital-100.pdf");
    auto canvas = window.canvas;
    ready(canvas, 0);
    const auto original = encodePdf(window.doc.pdf());
    canvas->setZoom(.5);
    canvas->goToPage(20);
    QCoreApplication::processEvents();
    check(canvas->visiblePages().size() >= 2, "two actual PDF pages visible together");
    const int beforePage = canvas->page;
    const double beforeZoom = canvas->zoom;
    QTest::keyClick(canvas->viewport(), Qt::Key_PageDown);
    QCoreApplication::processEvents();
    check(canvas->page > beforePage, "PageDown crosses a page boundary continuously");
    check(qAbs(canvas->zoom - beforeZoom) < .0001, "scrolling does not refit zoom");
    check(window.pages->currentRow() == canvas->page, "page list follows scroll");
    canvas->goToPage(49);
    ready(canvas, 49);
    check(canvas->visiblePages().contains(49), "50th page is visible");
    canvas->setZoom(1.3);
    canvas->goToPage(49);
    canvas->scrollBy(QPoint(0, 300));
    const auto position = canvas->anchor();
    QJsonArray errors;
    for (double scale : {1.8, 1.3, 2.0})
    {
        canvas->setZoom(scale);
        QCoreApplication::processEvents();
        double error = anchorError(canvas, position);
        errors.append(QJsonObject{{"zoom", scale}, {"max_axis_error_DIP", error}});
        check(error <= 2, QString("zoom anchor error %1 DIP").arg(error));
    }
    window.signatureAction->trigger();
    QTest::qWait(50);
    check(anchorError(canvas, position) <= 2, "properties panel preserves reading position");
    window.properties->hide();
    QTest::qWait(50);
    check(anchorError(canvas, position) <= 2, "closing properties restores reading position");
    window.resize(1100, 780);
    QTest::qWait(50);
    check(anchorError(canvas, position) <= 2, "resizing preserves reading position");
    check(!window.doc.dirty() && window.doc.cursor == 0 && encodePdf(window.doc.pdf()) == original,
          "viewing does not alter PDF or undo history");
    canvas->setZoom(.5);
    canvas->goToPage(49);
    ready(canvas, 49);
    window.grab().save(output + "/continuous-100-pages.png");
    canvas->fitWidth();
    canvas->setZoom(1.3);
    auto zoomMenu = window.statusBar()->findChild<QComboBox*>();
    check(zoomMenu != nullptr, "zoom menu is available in the status bar");
    zoomMenu->showPopup();
    QTest::keyClick(zoomMenu->view(), Qt::Key_Return);
    check(canvas->fitMode() == 1,
          "reselecting width in the zoom menu restores fit after a custom zoom");
    return {{"pages", 100}, {"zoom_anchor", errors}, {"source_and_history_unchanged", true}};
}

QJsonObject testViewerCoordinates(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    auto canvas = window.canvas;
    ready(canvas, 0);
    const auto original = encodePdf(window.doc.pdf());
    canvas->setZoom(.65);
    double referenceScale = -1;
    QJsonArray pages;
    for (int index = 0; index < window.doc.pages(); ++index)
    {
        canvas->goToPage(index);
        ready(canvas, index);
        const auto p = window.doc.pdf().getCatalog()->getPage(index);
        const auto crop = p->getCropBox();
        QPolygonF points;
        for (auto point : {crop.topLeft(), crop.topRight(), crop.bottomRight(), crop.bottomLeft()})
            points.append(canvas->pdfToViewport(index, point));
        auto bounds = points.boundingRect();
        auto size = pageSize(p);
        auto scaleX = bounds.width() / size.width();
        auto scaleY = bounds.height() / size.height();
        if (referenceScale < 0)
            referenceScale = scaleX;
        check(qAbs(scaleX - scaleY) < .005, "rotated CropBox preserves aspect ratio");
        check(qAbs(scaleX - referenceScale) < .005,
              "UserUnit is included in common document scale");
        for (auto point : {crop.center(), crop.topLeft() + QPointF(30, 40)})
        {
            auto actual = canvas->viewportToPdf(index, canvas->pdfToViewport(index, point));
            check(QLineF(point, actual).length() <= .5,
                  "PDF/display inverse within existing 0.5pt criterion");
        }
        pages.append(QJsonObject{{"page", index + 1},
                                 {"user_unit", p->getUserUnit()},
                                 {"scale_x", scaleX},
                                 {"scale_y", scaleY}});
        window.grab().save(output + QString("/coordinate-page-%1.png").arg(index + 1));
    }
    // Capture the entire crop for visual inspection as well as testing the
    // common physical scale above. A UserUnit=2 page exceeds this viewport.
    QJsonArray fits;
    for (int index = 0; index < window.doc.pages(); ++index)
    {
        canvas->goToPage(index);
        canvas->fitPage();
        canvas->goToPage(index);
        QCoreApplication::processEvents();
        ready(canvas, index);
        const auto crop = window.doc.pdf().getCatalog()->getPage(index)->getCropBox();
        QPolygonF corners;
        for (auto point : {crop.topLeft(), crop.topRight(), crop.bottomRight(), crop.bottomLeft()})
            corners.append(canvas->pdfToViewport(index, point));
        check(QRectF(canvas->viewport()->rect())
                  .adjusted(-2, -2, 2, 2)
                  .contains(corners.boundingRect()),
              QString("fit page contains all four CropBox corners, page %1").arg(index + 1));
        window.grab().save(output + QString("/coordinate-full-page-%1.png").arg(index + 1));
        auto bounds = corners.boundingRect();
        fits.append(QJsonObject{
            {"page", index + 1},
            {"zoom", canvas->zoom},
            {"bounds", QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()}},
            {"viewport", QJsonArray{canvas->viewport()->width(), canvas->viewport()->height()}}});
        canvas->viewport()->grab().save(output + QString("/viewport-full-%1.png").arg(index + 1));
    }
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty(),
          "rendering CropBox adaptation never changes the authoritative PDF");
    return {{"pages", pages}, {"fits", fits}, {"authoritative_pdf_unchanged", true}};
}

QJsonObject testViewerSignature(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    auto canvas = window.canvas;
    ready(canvas, 0, "initial open");
    window.signature->setPlainText("髙橋");
    window.size->setValue(12);
    canvas->setZoom(2);
    QJsonArray measured;
    for (int index = 0; index < window.doc.pages(); ++index)
    {
        canvas->goToPage(index);
        const auto crop = window.doc.pdf().getCatalog()->getPage(index)->getCropBox();
        ViewAnchor position{index, crop.center(), {.5, .5}};
        canvas->restoreAnchor(position);
        QCoreApplication::processEvents();
        window.grab().save(output + QString("/signature-position-%1.png").arg(index + 1));
        ready(canvas, index, "centered for placement");
        const auto expected = crop.center();
        const auto pixel = canvas->pdfToViewport(index, expected).toPoint();
        check(canvas->viewport()->rect().contains(pixel), "signature placement point is on screen");
        canvas->beginPlacement();
        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, pixel);
        const auto all = signatures(window.doc.pdf(), index);
        check(all.size() == 1, "pointer places one signature on the intended page");
        const auto error = QLineF(all[0].rect.topLeft(), expected).length();
        check(error <= .5, QString("actual signature placement error %1 pt").arg(error));
        window.undoAction->trigger();
        check(signatures(window.doc.pdf(), index).isEmpty(), "Undo removes only the new signature");
        window.redoAction->trigger();
        check(signatures(window.doc.pdf(), index).size() == 1,
              "Redo restores the same page signature");
        measured.append(QJsonObject{{"page", index + 1}, {"placement_error_pt", error}});
    }
    window.doc.save(output + "/viewer-signatures.pdf");
    for (int index = 0; index < window.doc.pages(); ++index)
    {
        canvas->goToPage(index);
        canvas->fitPage();
        canvas->goToPage(index);
        QCoreApplication::processEvents();
        ready(canvas, index);
        window.grab().save(output + QString("/signature-full-page-%1.png").arg(index + 1));
    }
    Document saved;
    saved.open(output + "/viewer-signatures.pdf");
    for (int index = 0; index < saved.pages(); ++index)
    {
        const auto all = signatures(saved.pdf(), index);
        check(all.size() == 1 && all[0].text == "髙橋",
              "saved signatures remain editable across rotations");
    }
    return {{"placements", measured}, {"saved_reeditable", true}};
}
} // namespace tatsu
