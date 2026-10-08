#include "layout_transition_tests.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool condition, const QString& message)
{
    if (!condition)
        fail(message);
}
void settle()
{
    QTest::qWait(70);
}
void ready(Window& window, int page)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 10000),
          "layout transition renders the actual page");
}
double anchorError(Window& window, const ViewAnchor& anchor)
{
    const auto actual = window.canvas->pdfToViewport(anchor.page, anchor.point);
    const QPointF expected(window.canvas->viewport()->width() * anchor.ratio.x(),
                           window.canvas->viewport()->height() * anchor.ratio.y());
    return qMax(qAbs(actual.x() - expected.x()), qAbs(actual.y() - expected.y()));
}
void record(const QString& path, const QJsonArray& rows)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "write layout diagnostics");
    file.write(QJsonDocument(rows).toJson());
}
} // namespace
QJsonObject testLayoutTransitions(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    const auto hash = fileHash(window.doc.source);
    auto navigation = window.findChild<QDockWidget*>("navigationDock");
    QJsonArray rows;
    for (int page = 1; page < window.doc.pages() - 1; ++page)
    {
        window.properties->hide();
        window.resize(1280, 850);
        settle();
        window.canvas->setZoom(2);
        window.canvas->goToPage(page);
        const auto crop = window.doc.pdf().getCatalog()->getPage(page)->getCropBox();
        window.canvas->restoreAnchor({page, crop.center(), {.5, .5}});
        ready(window, page);
        settle();
        auto anchor = window.canvas->anchor();
        auto step = [&](const QString& operation, const std::function<void()>& change)
        {
            change();
            settle();
            const auto error = anchorError(window, anchor);
            rows.append(QJsonObject{{"operation", operation},
                                    {"physical_page", page + 1},
                                    {"width", window.width()},
                                    {"height", window.height()},
                                    {"viewport_width", window.canvas->viewport()->width()},
                                    {"properties_width", window.properties->width()},
                                    {"navigation_visible", navigation->isVisible()},
                                    {"error_DIP", error}});
            record(output + "/layout-transition-steps.json", rows);
            check(error <= 2, QString("%1 lost page %2 reading position: %3 DIP")
                                  .arg(operation)
                                  .arg(page + 1)
                                  .arg(error));
            check(window.canvas->page == anchor.page && window.canvas->fitMode() == 0 &&
                      qAbs(window.canvas->zoom - 2) < .0001,
                  "manual zoom and page remain unchanged");
        };
        step("open signature", [&] { window.signatureAction->trigger(); });
        step("resize small", [&] { window.resize(800, 480); });
        step("close properties", [&] { window.properties->hide(); });
        step("resize wide", [&] { window.resize(1280, 850); });
        step("open OCR", [&] { window.ocrAction->trigger(); });
        step("resize burst",
             [&]
             {
                 for (const auto size : {QSize(1180, 790), QSize(960, 540), QSize(1280, 850),
                                         QSize(800, 480), QSize(1100, 780)})
                     window.resize(size);
             });
        step("close OCR", [&] { window.properties->hide(); });
    }
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty() &&
              window.doc.cursor == 0 && fileHash(window.doc.source) == hash,
          "resize and panel changes preserve PDF, edit history and source");
    window.grab().save(output + "/layout-transition-final.png");
    return {{"steps", rows},
            {"anchor_limit_DIP", 2},
            {"scope", "actual Qt layout; native OS DPI is separate"}};
}
QJsonObject testResizeNavigationInput(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D10-digital-100.pdf");
    window.activateWindow();
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    window.canvas->setZoom(2);
    window.canvas->goToPage(49);
    ready(window, 49);
    window.signatureAction->trigger();
    settle();
    window.signature->setPlainText("山田 太郎\n入力は保持する");
    window.signature->setFocus();
    window.signature->moveCursor(QTextCursor::End);
    QInputMethodEvent composing("にほんご", {});
    QApplication::sendEvent(window.signature, &composing);
    for (const auto size : {QSize(800, 480), QSize(1280, 850), QSize(960, 540)})
        window.resize(size);
    settle();
    check(window.signature->hasFocus() &&
              window.signature->toPlainText() == "山田 太郎\n入力は保持する",
          "resize retains signature input and composition focus");
    QInputMethodEvent commit;
    commit.setCommitString("日本語");
    QApplication::sendEvent(window.signature, &commit);
    check(window.signature->toPlainText() == "山田 太郎\n入力は保持する日本語",
          "Japanese composition can commit after resize");
    const auto input = window.signature->toPlainText();
    window.ocrAction->trigger();
    window.signatureAction->trigger();
    settle();
    check(window.signature->toPlainText() == input, "panel roundtrip retains entered signature");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_L, Qt::ControlModifier);
    auto number = window.findChild<QLineEdit*>("pageNumber");
    number->setText("75");
    window.resize(1280, 850);
    window.resize(800, 480);
    settle();
    check(number->hasFocus() && number->text() == "75",
          "resize never overwrites unfinished page-number input");
    QTest::keyClick(number, Qt::Key_Return);
    ready(window, 74);
    check(window.canvas->page == 74, "unfinished input remains actionable after resize");
    for (int page : {20, 49, 70, 30})
    {
        window.resize(window.width() < 1200 ? QSize(1280, 850) : QSize(800, 480));
        window.canvas->goToPage(page);
        ready(window, page);
        settle();
        check(window.canvas->page == page,
              "explicit navigation supersedes queued resize restoration");
    }
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty() && window.doc.cursor == 0,
          "input drafts and resize never edit the PDF");
    window.grab().save(output + "/resize-input-final.png");
    return {{"signature_draft_and_composition", true},
            {"unfinished_page_number", true},
            {"explicit_navigation_cases", 4},
            {"native_IME", "未実行; QInputMethodEvent"}};
}
QJsonObject testAutomaticFit(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    const auto sourceHash = fileHash(window.doc.source);
    QJsonArray rows;
    for (const auto size : {QSize(1280, 850), QSize(1024, 720), QSize(960, 540), QSize(800, 480)})
        for (bool settings : {false, true})
        {
            window.resize(size);
            if (settings)
                window.signatureAction->trigger();
            else
                window.properties->hide();
            settle();
            for (int mode : {1, 2})
                for (int page = 0; page < window.doc.pages(); ++page)
                {
                    window.canvas->goToPage(page);
                    if (mode == 1)
                        window.canvas->fitWidth();
                    else
                        window.canvas->fitPage();
                    window.canvas->goToPage(page);
                    ready(window, page);
                    settle();
                    const auto crop = window.doc.pdf().getCatalog()->getPage(page)->getCropBox();
                    QPolygonF corners;
                    for (const auto point :
                         {crop.topLeft(), crop.topRight(), crop.bottomRight(), crop.bottomLeft()})
                        corners.append(window.canvas->pdfToViewport(page, point));
                    const auto bounds = corners.boundingRect();
                    const auto viewport = QRectF(window.canvas->viewport()->rect());
                    const bool fits =
                        mode == 1 ? bounds.left() >= -2 && bounds.right() <= viewport.right() + 2
                                  : viewport.adjusted(-2, -2, 2, 2).contains(bounds);
                    rows.append(
                        QJsonObject{{"physical_page", page + 1},
                                    {"fit_mode", mode},
                                    {"settings", settings},
                                    {"window", QJsonArray{size.width(), size.height()}},
                                    {"viewport", QJsonArray{viewport.width(), viewport.height()}},
                                    {"zoom", window.canvas->zoom},
                                    {"bounds", QJsonArray{bounds.x(), bounds.y(), bounds.width(),
                                                          bounds.height()}},
                                    {"fits", fits}});
                    record(output + "/automatic-fit-steps.json", rows);
                    if (!fits)
                        window.grab().save(output + "/automatic-fit-failure.png");
                    check(
                        fits,
                        QString("fit mode %1 page %2 does not fit %3x%4 with settings %5, zoom %6")
                            .arg(mode)
                            .arg(page + 1)
                            .arg(size.width())
                            .arg(size.height())
                            .arg(settings)
                            .arg(window.canvas->zoom));
                    check(window.canvas->viewState().fitReference == page &&
                              window.canvas->fitMode() == mode,
                          "automatic fit uses the explicitly selected reference page");
                }
        }
    check(window.canvas->zoom < .25, "large page uses an automatic fit below the manual floor");
    const auto automatic = window.canvas->zoom;
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Minus, Qt::ControlModifier);
    check(window.canvas->zoom <= automatic && window.canvas->fitMode() == 0,
          "zoom out from a low automatic fit never zooms in");
    const auto lowManualView = window.canvas->viewState();
    const auto mouse = QPointF(window.canvas->viewport()->rect().center());
    QWheelEvent wheel(mouse, window.canvas->viewport()->mapToGlobal(mouse.toPoint()), {},
                      QPoint(0, -120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(window.canvas->viewport(), &wheel);
    check(window.canvas->zoom <= automatic, "Ctrl+wheel down never increases a low automatic zoom");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Plus, Qt::ControlModifier);
    check(window.canvas->zoom >= automatic, "zoom in moves towards the manual range");
    window.canvas->setZoom(1);
    window.canvas->restoreView(lowManualView);
    settle();
    check(qAbs(window.canvas->zoom - lowManualView.zoom) < .0001 &&
              window.canvas->fitMode() == lowManualView.fitMode,
          "history restores an actual zoom below the manual range");
    window.canvas->setZoom(.01);
    check(window.canvas->zoom == .25, "explicit manual zoom retains its lower limit");
    window.canvas->setZoom(8);
    check(window.canvas->zoom == 4, "explicit manual zoom retains its upper limit");
    window.canvas->fitPage();
    window.canvas->goToPage(3);
    window.resize(1280, 850);
    settle();
    window.resize(800, 480);
    settle();
    ready(window, 3);
    const auto crop = window.doc.pdf().getCatalog()->getPage(3)->getCropBox();
    QPolygonF corners;
    for (const auto point :
         {crop.topLeft(), crop.topRight(), crop.bottomRight(), crop.bottomLeft()})
        corners.append(window.canvas->pdfToViewport(3, point));
    const auto bounds = corners.boundingRect();
    const auto position = window.canvas->anchor();
    rows.append(
        QJsonObject{{"operation", "wide-to-small resize"},
                    {"current_page", window.canvas->page + 1},
                    {"reference_page", window.canvas->viewState().fitReference + 1},
                    {"fit_mode", window.canvas->fitMode()},
                    {"zoom", window.canvas->zoom},
                    {"bounds", QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()}},
                    {"anchor_page", position.page + 1},
                    {"anchor_point", QJsonArray{position.point.x(), position.point.y()}},
                    {"viewport", QJsonArray{window.canvas->viewport()->width(),
                                            window.canvas->viewport()->height()}}});
    record(output + "/automatic-fit-steps.json", rows);
    window.grab().save(output + "/automatic-fit-resize.png");
    check(QRectF(window.canvas->viewport()->rect())
                  .adjusted(-2, -2, 2, 2)
                  .contains(corners.boundingRect()) &&
              window.canvas->fitMode() == 2 && window.canvas->viewState().fitReference == 3,
          "active automatic fit recalculates through a wide-to-small resize");
    const auto fittedView = window.canvas->viewState();
    window.canvas->restoreAnchor(
        {1, window.doc.pdf().getCatalog()->getPage(1)->getCropBox().center(), {.5, .5}});
    ready(window, 1);
    window.resize(1280, 850);
    settle();
    window.resize(800, 480);
    settle();
    ready(window, 1);
    check(window.canvas->page == 1 && window.canvas->viewState().fitReference == 3 &&
              window.canvas->fitMode() == 2,
          "resize after continuous scrolling never pulls the view back to the fit reference");
    window.canvas->restoreView(fittedView);
    ready(window, 3);
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty() &&
              window.doc.cursor == 0 && fileHash(window.doc.source) == sourceHash,
          "automatic fit preserves PDF, edit history and source");
    window.grab().save(output + "/automatic-fit-small.png");
    return {{"cases", rows},
            {"corner_tolerance_DIP", 2},
            {"resize_recalculates_fit", true},
            {"relative_zoom_direction", true},
            {"low_zoom_history", true}};
}
} // namespace tatsu
