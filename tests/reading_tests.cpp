#include "reading_tests.h"
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
void ready(Window& window, int page)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 10000),
          QString("reading page %1 is rendered").arg(page + 1));
}
QLineEdit* number(Window& window)
{
    return window.findChild<QLineEdit*>("pageNumber");
}
QAction* mode(Window& window)
{
    return window.findChild<QAction*>("readingMode");
}
void focusNumber(Window& window)
{
    window.activateWindow();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_L, Qt::ControlModifier);
    check(number(window)->hasFocus(), "Ctrl+L focuses the page input");
}
void request(Window& window, const QString& text)
{
    focusNumber(window);
    number(window)->setText(text);
    QTest::keyClick(number(window), Qt::Key_Return);
    QCoreApplication::processEvents();
}
double error(Window& window, const ViewAnchor& anchor)
{
    const auto actual = window.canvas->pdfToViewport(anchor.page, anchor.point);
    const QPointF expected(window.canvas->viewport()->width() * anchor.ratio.x(),
                           window.canvas->viewport()->height() * anchor.ratio.y());
    return qMax(qAbs(actual.x() - expected.x()), qAbs(actual.y() - expected.y()));
}
void settle()
{
    QTest::qWait(50);
}
} // namespace
QJsonObject testReadingInitialPanels(const QString& fixtures, const QString& output)
{
    QJsonArray cases;
    for (const bool beforeShow : {false, true})
        for (const int panel : {0, 1})
        {
            Window window;
            window.resize(1024, 720);
            if (!beforeShow)
                window.show();
            window.openFile(fixtures + "/D01.pdf");
            const auto original = encodePdf(window.doc.pdf());
            (panel == 0 ? window.signatureAction : window.ocrAction)->trigger();
            if (beforeShow)
                window.show();
            ready(window, 0);
            settle();
            bool japanese = false, english = false;
            for (const auto& flow : PDFTextFlow::createTextFlows(textLayout(window.doc.pdf(), 0),
                                                                 PDFTextFlow::AddLineBreaks, 0))
                for (const auto& word : {QString("日本語"), QString("English")})
                    if (const auto index = flow.getText().indexOf(word); index >= 0)
                    {
                        const auto center = flow.getBoundingBoxes()[size_t(index)].center();
                        const bool visible = window.canvas->viewport()->rect().contains(
                            window.canvas->pdfToViewport(0, center).toPoint());
                        if (word == "日本語")
                            japanese = visible;
                        else
                            english = visible;
                    }
            window.grab().save(output +
                               QString("/initial-panel-%1-%2.png").arg(beforeShow).arg(panel));
            check(japanese && english, "opening a panel immediately must show the first PDF text");
            check(!window.doc.dirty() && window.doc.cursor == 0 &&
                      encodePdf(window.doc.pdf()) == original,
                  "initial panel changes no PDF or Undo state");
            cases.append(QJsonObject{{"open_before_show", beforeShow},
                                     {"panel", panel},
                                     {"Japanese_and_English_visible", true}});
        }
    Window window;
    window.show();
    window.openFile(fixtures + "/D10-digital-100.pdf");
    window.signatureAction->trigger();
    window.pages->setCurrentRow(49);
    ready(window, 49);
    settle();
    check(window.canvas->page == 49, "explicit navigation wins over pending initial panel layout");
    return {{"cases", cases}, {"explicit_navigation_wins", true}};
}
QJsonObject testReadingImageNavigation(const QString& fixtures, const QString& output)
{
    const auto source = fixtures + "/D10-image-50.pdf";
    const auto hash = fileHash(source);
    Window window;
    window.resize(1280, 850);
    window.show();
    window.openFile(source);
    window.canvas->fitPage();
    // Compare the same navigation route and completed viewport, rather than
    // the initial fit anchor against an explicit jump (integer scroll rounding).
    window.canvas->goToPage(0);
    auto completeViewport = [&]
    {
        check(QTest::qWaitFor(
                  [&]
                  {
                      const auto visible = window.canvas->visiblePages();
                      return !visible.isEmpty() &&
                             std::all_of(visible.begin(), visible.end(), [&](int number)
                                         { return window.canvas->pageReady(number); });
                  },
                  15000),
              "all visible image pages finish");
    };
    ready(window, 0);
    completeViewport();
    settle();
    const auto original = encodePdf(window.doc.pdf());
    const auto revision = window.doc.revision;
    const auto first = window.canvas->viewport()->grab().toImage();
    const auto initialAnchor = window.canvas->anchor();
    first.save(output + "/image-navigation-first.png");
    int ticks = 0;
    QTimer pulse;
    QObject::connect(&pulse, &QTimer::timeout, [&] { ++ticks; });
    pulse.start(10);
    QJsonArray destinations;
    for (const int page : {1, 2, 49, 0, 24, 23, 24, 0})
    {
        window.canvas->goToPage(page);
        ready(window, page);
        completeViewport();
        if (page == 0)
            window.canvas->restoreAnchor(initialAnchor);
        QTest::qWait(80);
        const auto image = window.canvas->viewport()->grab().toImage();
        check(!image.isNull(), "image document still draws after forward/backward jumps");
        if (page == 0)
        {
            image.save(output + "/image-navigation-return.png");
            const auto current = window.canvas->anchor();
            QFile diagnostic(output + "/image-navigation-anchors.json");
            diagnostic.open(QIODevice::WriteOnly);
            diagnostic.write(QJsonDocument(QJsonObject{{"initial_page", initialAnchor.page},
                                                       {"returned_page", current.page},
                                                       {"initial_x", initialAnchor.point.x()},
                                                       {"initial_y", initialAnchor.point.y()},
                                                       {"returned_x", current.point.x()},
                                                       {"returned_y", current.point.y()},
                                                       {"same_pixels", image == first}})
                                 .toJson());
            check(image == first, "returning to an evicted image page draws identical pixels");
        }
        destinations.append(page + 1);
    }
    // Old background work must not take priority over the final destination.
    for (const int page : {49, 1, 24, 0})
        window.canvas->goToPage(page);
    ready(window, 0);
    completeViewport();
    window.canvas->restoreAnchor(initialAnchor);
    QTest::qWait(150);
    check(window.canvas->page == 0 && window.canvas->viewport()->grab().toImage() == first,
          "rapid distant jumps converge to the requested page without stale content");
    pulse.stop();
    check(ticks > 0 && !window.doc.dirty() && window.doc.cursor == 0 &&
              window.doc.revision == revision && encodePdf(window.doc.pdf()) == original &&
              fileHash(source) == hash,
          "image reading responds and preserves the document, Undo and source");
    window.grab().save(output + "/reading-image-navigation.png");
    return {{"destinations", destinations},
            {"UI_timer_ticks", ticks},
            {"return_pixels_identical", true},
            {"rapid_jumps_final_destination", true},
            {"PDF_Undo_source_unchanged", true}};
}
QJsonObject testReadingCompilerStartup(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    QElapsedTimer time;
    time.start();
    window.openFile(fixtures + "/D01.pdf");
    ready(window, 0);
    check(window.canvas->visiblePages().contains(0) && !window.doc.dirty() &&
              window.doc.cursor == 0,
          "single-page startup paints real content without editing");
    window.grab().save(output + "/compiler-startup.png");
    return {{"ready_ms", time.elapsed()},
            {"single_page_rendered", true},
            {"PDF_and_Undo_unchanged", true}};
}
QJsonObject testReadingPageInput(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D10-digital-100.pdf");
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    window.activateWindow();
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_End, Qt::ControlModifier);
    ready(window, 99);
    check(window.canvas->page == 99, "Ctrl+End reaches the last physical page");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Home, Qt::ControlModifier);
    ready(window, 0);
    check(window.canvas->page == 0, "Ctrl+Home reaches the first physical page");
    request(window, "50");
    ready(window, 49);
    check(window.canvas->page == 49 && number(window)->text() == "50", "physical page 50");
    const auto position = window.canvas->anchor();
    window.findChild<QAction*>("previousView")->trigger();
    settle();
    check(window.canvas->page == 0, "page input adds a view history entry");
    window.findChild<QAction*>("nextView")->trigger();
    settle();
    check(error(window, position) <= 2, "forward restores the page input destination");
    window.canvas->setZoom(1);
    const auto zoomAnchor = window.canvas->anchor();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Plus, Qt::ControlModifier);
    check(qAbs(window.canvas->zoom - 1.2) < .001 && error(window, zoomAnchor) <= 2,
          "Ctrl+Plus zooms at the reading anchor");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Minus, Qt::ControlModifier);
    check(qAbs(window.canvas->zoom - 1) < .001, "Ctrl+Minus returns to the previous zoom");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Plus,
                    Qt::ControlModifier | Qt::ShiftModifier);
    check(qAbs(window.canvas->zoom - 1.2) < .001, "Shift needed to type Plus does not block zoom");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_0, Qt::ControlModifier);
    check(window.canvas->fitMode() == 2, "Ctrl+0 fits the whole page");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_1, Qt::ControlModifier);
    check(window.canvas->fitMode() == 1, "Ctrl+1 fits the width");
    QJsonArray rejected;
    for (const QString& text : {QString(), QString("0"), QString("101"), QString("-1"),
                                QString("abc"), QString("999999999999")})
    {
        const auto before = window.canvas->anchor();
        request(window, text);
        check(number(window)->hasFocus() && number(window)->property("invalidPage").toBool(),
              "invalid page stays in the input with a visible explanation");
        check(error(window, before) <= 2 && window.canvas->page == 49,
              "invalid page must not move or clamp");
        rejected.append(text);
    }
    window.grab().save(output + "/reading-invalid-page.png");
    QTest::keyClick(number(window), Qt::Key_Escape);
    check(number(window)->text() == "50" && !number(window)->hasFocus(),
          "Esc restores current page");
    focusNumber(window);
    number(window)->setText("75");
    window.canvas->scrollBy({0, 900});
    check(number(window)->text() == "75", "scrolling does not overwrite unfinished input");
    QTest::keyClick(number(window), Qt::Key_Return);
    ready(window, 74);
    check(window.canvas->page == 74, "unfinished input remains actionable");
    focusNumber(window);
    number(window)->setText("20");
    QInputMethodEvent composition("にじゅう", {});
    QApplication::sendEvent(number(window), &composition);
    QTest::keyClick(number(window), Qt::Key_Return);
    check(window.canvas->page == 74, "IME preedit Enter is not a page jump");
    QInputMethodEvent clear;
    QApplication::sendEvent(number(window), &clear);
    QTest::keyClick(number(window), Qt::Key_Escape);
    check(!window.doc.dirty() && window.doc.cursor == 0 && encodePdf(window.doc.pdf()) == original,
          "page input and history never edit the PDF");
    window.openFile(fixtures + "/viewer-navigation.pdf");
    request(window, "5");
    ready(window, 4);
    check(window.canvas->page == 4 && window.status->text().contains("AA"),
          "PageLabels are displayed but physical number 5 selects page 5");
    window.grab().save(output + "/reading-page-label.png");
    return {{"physical_pages", 100},
            {"rejected_inputs", rejected},
            {"IME", "QInputMethodEvent; native OS IME is a separate test"}};
}
QJsonObject testReadingLayout(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/viewer-navigation.pdf");
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    auto navigation = window.findChild<QDockWidget*>("navigationDock");
    auto toolbar = window.findChild<QToolBar*>("documentToolbar");
    QJsonArray errors;
    for (int page : {0, 1, 2, 3, 5})
    {
        window.canvas->setZoom(2);
        window.canvas->goToPage(page);
        window.canvas->restoreAnchor({page, {300, 400}, {.5, .5}});
        ready(window, page);
        const auto anchor = window.canvas->anchor();
        mode(window)->trigger();
        settle();
        check(mode(window)->isChecked() && !navigation->isVisible() && !toolbar->isVisible(),
              "focus mode hides both docks and document toolbar");
        auto maximum = error(window, anchor);
        check(maximum <= 2, "entering focus mode preserves the PDF point");
        QTest::keyClick(window.canvas->viewport(), Qt::Key_Escape);
        settle();
        maximum = qMax(maximum, error(window, anchor));
        check(!mode(window)->isChecked() && navigation->isVisible() && toolbar->isVisible() &&
                  maximum <= 2,
              "Esc restores the previous reading layout and PDF point");
        errors.append(QJsonObject{{"physical_page", page + 1}, {"max_axis_error_DIP", maximum}});
    }
    window.resize(1024, 720);
    settle();
    window.signatureAction->trigger();
    settle();
    check(window.properties->isVisible() && !navigation->isVisible(),
          "narrow window shows properties without competing left panel");
    const auto narrow = window.canvas->anchor();
    window.grab().save(output + "/reading-1024-signature.png");
    mode(window)->trigger();
    settle();
    check(!window.properties->isVisible() && error(window, narrow) <= 2,
          "focus mode hides properties and keeps the narrow reading anchor");
    ready(window, window.canvas->page);
    window.grab().save(output + "/reading-1024-focus.png");
    focusNumber(window);
    QTest::keyClick(number(window), Qt::Key_F8);
    check(mode(window)->isChecked(), "F8 in an input does not toggle reading mode");
    QTest::keyClick(number(window), Qt::Key_Escape);
    check(mode(window)->isChecked(),
          "first Esc returns from page input without closing focus mode");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Escape);
    settle();
    check(window.properties->isVisible() && !navigation->isVisible() && error(window, narrow) <= 2,
          "focus exit restores the signature panel in a narrow window");
    window.properties->hide();
    settle();
    check(navigation->isVisible(), "closing properties restores page navigation");
    window.resize(1280, 850);
    settle();
    window.signatureAction->trigger();
    settle();
    check(navigation->isVisible() && window.properties->isVisible(),
          "wide window shows both panels");
    mode(window)->trigger();
    settle();
    window.canvas->beginPlacement();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Escape);
    check(mode(window)->isChecked() && !window.canvas->placing,
          "Esc cancels placement before leaving focus mode");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Escape);
    settle();
    check(!mode(window)->isChecked(), "second Esc leaves focus mode");
    check(!window.doc.dirty() && window.doc.cursor == 0 && encodePdf(window.doc.pdf()) == original,
          "panels, focus mode and cancelled placement do not edit the document");
    mode(window)->trigger();
    settle();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    settle();
    check(!mode(window)->isChecked() && navigation->isVisible() && window.query->hasFocus(),
          "Ctrl+F exits focus mode and keeps the search entry visible");
    mode(window)->trigger();
    window.openFile(fixtures + "/D01.pdf");
    settle();
    check(!mode(window)->isChecked() && toolbar->isVisible(), "another document resets focus mode");
    return {{"anchor_errors", errors},
            {"window_sizes", "1024x720; 1280x850"},
            {"native_DPI", "未実行; Qt logical dimensions"}};
}
QJsonObject testReadingOcrCancel(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D03.pdf");
    window.doc.putSignature(0, "保持する署名", {50, 40}, 16, Qt::black);
    window.refresh();
    ready(window, 0);
    const auto before = window.doc.pdf();
    const auto revision = window.doc.revision;
    mode(window)->trigger();
    settle();
    window.startOcr();
    const auto temporary = window.work->path();
    check(
        QTest::qWaitFor(
            [&] { return window.worker && window.progress->text().startsWith("OCR 1 /"); }, 90000),
        "real OCR worker reaches partial progress");
    request(window, "2");
    ready(window, 1);
    check(window.canvas->page == 1 && mode(window)->isChecked() && window.cancel->isVisible(),
          "page navigation and cancellation stay available while OCR runs in focus mode");
    QTest::mouseClick(window.cancel, Qt::LeftButton);
    check(QTest::qWaitFor([&] { return !window.worker; }, 10000), "OCR cancellation completes");
    check(!window.doc.busy && window.doc.pdf() == before && window.doc.revision == revision &&
              window.doc.dirty() && !QFileInfo::exists(temporary),
          "focus mode OCR cancellation preserves unsaved signature and removes temporary job");
    window.grab().save(output + "/reading-ocr-cancel.png");
    window.doc.save(output + "/reading-cancel-saved.pdf");
    const auto savedSignature = signatures(window.doc.pdf(), 0).front();
    window.doc.moveSignature(0, savedSignature, {20, 10});
    window.refresh();
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Z, Qt::ControlModifier);
    check(signatures(window.doc.pdf(), 0).front().rect == savedSignature.rect &&
              !window.doc.dirty(),
          "Ctrl+Z remains available with the document toolbar hidden");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(signatures(window.doc.pdf(), 0).front().rect != savedSignature.rect && window.doc.dirty(),
          "Ctrl+Shift+Z redoes the signature move in focus mode");
    const auto hash = fileHash(output + "/reading-cancel-saved.pdf");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_S, Qt::ControlModifier);
    check(!window.doc.dirty() && fileHash(output + "/reading-cancel-saved.pdf") != hash,
          "Ctrl+S saves real PDF changes with the document toolbar hidden");
    Document reopened;
    reopened.open(output + "/reading-cancel-saved.pdf");
    const auto signature = signatures(reopened.pdf(), 0).front();
    reopened.moveSignature(0, signature, {20, 10});
    reopened.undo();
    check(signatures(reopened.pdf(), 0).front().text == "保持する署名",
          "saved signature remains editable");
    window.doc.saved = window.doc.cursor;
    return {{"OCR", "real jpn+eng worker, cancellation after first completed page"},
            {"focus_mode_navigation", "PASS"},
            {"unsaved_signature_retained", true},
            {"temporary_removed", true},
            {"saved_signature_reeditable", true}};
}
} // namespace tatsu
