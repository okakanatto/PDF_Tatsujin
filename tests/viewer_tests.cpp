#include "viewer_tests.h"
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
