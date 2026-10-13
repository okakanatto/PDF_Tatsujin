#include "pan_tests.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
void ready(Window& window, int page = 0)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 10000),
          QString("actual PDF page %1 is ready (%2, current=%3)")
              .arg(page + 1)
              .arg(QFileInfo(window.doc.source).fileName())
              .arg(window.canvas->page + 1));
}
QAction* action(Window& window, const char* name)
{
    auto result = window.findChild<QAction*>(name);
    check(result != nullptr, QString("action exists: %1").arg(name));
    return result;
}
void focus(Window& window)
{
    window.activateWindow();
    window.canvas->setFocus();
    QCoreApplication::processEvents();
    check(window.canvas->viewport()->hasFocus(), "PDF viewport has keyboard focus");
}
QPoint scroll(Window& window)
{
    return {window.canvas->horizontalScrollBar()->value(),
            window.canvas->verticalScrollBar()->value()};
}
void drag(Window& window, QPoint start, QPoint end)
{
    auto viewport = window.canvas->viewport();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(viewport, end, 10);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
}
void center(Window& window, int page = 0)
{
    auto canvas = window.canvas;
    canvas->goToPage(page);
    canvas->setZoom(2);
    canvas->restoreAnchor(
        {page, window.doc.pdf().getCatalog()->getPage(page)->getCropBox().center(), {.5, .5}});
    ready(window, page);
}
} // namespace

QJsonObject testPanNavigation(const QString& fixtures, const QString& output)
{
    Window window;
    check(!action(window, "handReadingTool")->isEnabled(), "hand tool disabled before open");
    window.resize(1024, 720);
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    ready(window);
    const auto original = encodePdf(window.doc.pdf());
    QJsonArray errors;
    for (int page = 0; page < 4; ++page)
    {
        center(window, page);
        action(window, "handReadingTool")->trigger();
        auto canvas = window.canvas;
        check(canvas->viewport()->cursor().shape() == Qt::OpenHandCursor, "open hand cursor");
        const auto anchor = canvas->anchor();
        const auto before = canvas->pdfToViewport(page, anchor.point);
        const QPoint start = canvas->viewport()->rect().center();
        const QPoint delta(40, 35);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
        check(canvas->viewport()->cursor().shape() == Qt::ClosedHandCursor, "drag cursor");
        QTest::mouseMove(canvas->viewport(), start + delta, 10);
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start + delta);
        const auto actual = canvas->pdfToViewport(page, anchor.point) - before;
        const auto error = qMax(qAbs(actual.x() - delta.x()), qAbs(actual.y() - delta.y()));
        check(error <= 2, QString("pan follows pointer at rotation/UserUnit: %1 DIP").arg(error));
        errors.append(QJsonObject{{"page", page + 1}, {"max_axis_error_DIP", error}});
        const auto stopped = scroll(window);
        QTest::mouseMove(canvas->viewport(), start);
        check(scroll(window) == stopped, "release stops movement");
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
        const auto fractionalStart = scroll(window);
        for (int step = 1; step <= 25; ++step)
        {
            const QPointF point = QPointF(start) + QPointF(.4 * step, -.4 * step);
            QMouseEvent move(QEvent::MouseMove, point, canvas->viewport()->mapToGlobal(point),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
        }
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                            start + QPoint(10, -10));
        check(scroll(window) - fractionalStart == QPoint(-10, 10),
              "25 fractional 0.4 DIP motions accumulate to exactly 10 DIP per axis");
    }
    for (auto name : {"selectReadingTool", "handReadingTool"})
        for (auto object : action(window, name)->associatedObjects())
            if (auto button = qobject_cast<QToolButton*>(object))
                check(button->isVisible() && window.rect().contains(QRect(
                                                 button->mapTo(&window, QPoint()), button->size())),
                      "reading controls fit 1024 by 720");
    window.grab().save(output + "/hand-1024.png");
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty() && window.doc.cursor == 0,
          "panning never changes PDF or edit undo");
    check(!action(window, "previousView")->isEnabled(), "panning does not add view history");

    window.openFile(fixtures + "/D10-digital-100.pdf");
    ready(window);
    check(!window.canvas->handToolActive(), "new PDF resets to selection");
    window.canvas->setZoom(.4);
    window.canvas->goToPage(20);
    ready(window, 20);
    const auto previousPage = window.canvas->page;
    action(window, "handReadingTool")->trigger();
    auto viewport = window.canvas->viewport();
    drag(window, {viewport->width() / 2, viewport->height() - 40}, {viewport->width() / 2, 40});
    check(window.canvas->page > previousPage, "pan crosses page boundary continuously");
    check(window.pages->currentRow() == window.canvas->page, "page list follows panning");
    const auto gapPosition = scroll(window);
    drag(window, {3, viewport->height() / 2}, {3, viewport->height() / 2 - 40});
    check(scroll(window).y() - gapPosition.y() == 40, "hand also starts in page margin");
    auto bar = window.canvas->verticalScrollBar();
    bar->setValue(bar->maximum());
    const auto start = viewport->rect().center();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(viewport, start - QPoint(0, 80), 10);
    check(bar->value() == bar->maximum(), "document end clamps scrolling");
    QTest::mouseMove(viewport, start - QPoint(0, 70), 10);
    check(bar->maximum() - bar->value() == 10, "reversing at edge responds immediately");
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start - QPoint(0, 70));

    window.doc.open(fixtures + "/viewer-selection-restricted.pdf", "selection-user");
    window.refresh(true);
    center(window);
    action(window, "handReadingTool")->trigger();
    const auto restricted = window.doc.pdf();
    const auto sourceHash = fileHash(window.doc.source);
    const auto revision = window.doc.revision;
    // Encryption can use fresh random bytes on every serialization. Compare
    // the actual object storage and original file, not two encrypted outputs.
    const bool repeatedEncodingEqual = encodePdf(restricted) == encodePdf(restricted);
    const auto position = scroll(window);
    drag(window, start, start - QPoint(20, 40));
    check(scroll(window) != position && !window.doc.copyAllowed && !window.doc.readOnly.isEmpty(),
          "copy-restricted read-only PDF still permits panning");
    check(window.doc.pdf() == restricted && window.doc.cursor == 0 && !window.doc.dirty() &&
              window.doc.revision == revision && fileHash(window.doc.source) == sourceHash,
          "protected PDF is unmodified");
    return {{"coordinates", errors},
            {"page_boundary", true},
            {"read_only", true},
            {"same_encrypted_document_serialized_twice_equal", repeatedEncodingEqual},
            {"minimum_window", "1024x720 Qt offscreen; actual OS DPI 未実行"}};
}

QJsonObject testPanInput(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/viewer-search.pdf");
    window.canvas->setZoom(.5);
    ready(window);
    auto canvas = window.canvas;
    auto viewport = canvas->viewport();
    const auto crop = window.doc.pdf().getCatalog()->getPage(0)->getCropBox();
    drag(window, canvas->pdfToViewport(0, crop.bottomLeft()).toPoint(),
         canvas->pdfToViewport(0, crop.topRight()).toPoint());
    check(QTest::qWaitFor([&] { return canvas->selectionReady(); }, 10000), "text loaded");
    const auto selection = canvas->copied;
    check(!selection.isEmpty(), "completed text selection exists before temporary hand");
    focus(window);
    QTest::keyPress(viewport, Qt::Key_Space);
    check(canvas->handToolActive() && action(window, "handReadingTool")->isChecked(),
          "Space temporarily activates visible hand state");
    const auto start = viewport->rect().center();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(viewport, start - QPoint(0, 40), 10);
    for (auto type : {QEvent::KeyRelease, QEvent::KeyPress})
    {
        QKeyEvent repeat(type, Qt::Key_Space, Qt::NoModifier, " ", true);
        QApplication::sendEvent(viewport, &repeat);
    }
    check(viewport->cursor().shape() == Qt::ClosedHandCursor, "auto-repeat preserves drag");
    QTest::keyRelease(viewport, Qt::Key_Space);
    const auto stopped = scroll(window);
    QTest::mouseMove(viewport, start - QPoint(0, 90), 10);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start - QPoint(0, 90));
    check(scroll(window) == stopped && !canvas->handToolActive(), "Space release stops drag");
    check(canvas->copied == selection, "temporary panning retains completed text selection");
    QTest::keyClick(viewport, Qt::Key_C, Qt::ControlModifier);
    check(QApplication::clipboard()->text() == selection, "retained range remains copyable");
    action(window, "handReadingTool")->trigger();
    QTest::keyClick(viewport, Qt::Key_Space);
    check(canvas->handToolActive(), "temporary release restores persistent hand");
    QTest::keyClick(viewport, Qt::Key_Escape);
    check(!canvas->handToolActive() && action(window, "selectReadingTool")->isChecked(),
          "Escape visibly returns to selection");
    window.query->setText("English");
    window.query->setFocus();
    window.query->setCursorPosition(window.query->text().size());
    QTest::keyClick(window.query, Qt::Key_Space);
    check(window.query->text() == "English " && !canvas->handToolActive(),
          "search editor receives Space");
    window.signatureAction->trigger();
    window.signature->setPlainText("山田");
    window.signature->moveCursor(QTextCursor::End);
    QTest::keyClick(window.signature, Qt::Key_Space);
    check(window.signature->toPlainText() == "山田 " && !canvas->handToolActive(),
          "signature editor receives Space");
    focus(window);
    QTest::keyClick(viewport, Qt::Key_Space, Qt::ControlModifier);
    check(!canvas->handToolActive(), "modified Space is not captured");
    window.grab().save(output + "/hand-input.png");
    return {{"Space_repeat_release", true},
            {"retained_copy_characters", selection.size()},
            {"editor_space", true},
            {"native_IME", "未実行; synthetic Qt key events"}};
}

QJsonObject testPanLifecycle(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D01.pdf");
    ready(window);
    const auto crop = window.doc.pdf().getCatalog()->getPage(0)->getCropBox();
    auto signature = window.doc.putSignature(0, "山田 太郎", crop.center(), 14, Qt::black);
    window.refresh();
    center(window);
    auto canvas = window.canvas;
    auto viewport = canvas->viewport();
    const auto original = encodePdf(window.doc.pdf());
    const int cursor = window.doc.cursor;
    action(window, "handReadingTool")->trigger();
    auto start = canvas->pdfToViewport(0, signature.rect.center()).toPoint();
    check(viewport->rect().contains(start), "signature is visible for hand drag");
    drag(window, start, start - QPoint(30, 35));
    check(encodePdf(window.doc.pdf()) == original && window.doc.cursor == cursor,
          "hand over signature does not move or select it");
    action(window, "selectReadingTool")->trigger();
    start = canvas->pdfToViewport(0, signature.rect.center()).toPoint();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(viewport, start + QPoint(35, 30), 10);
    QTest::keyPress(viewport, Qt::Key_Space);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start + QPoint(35, 30));
    QTest::keyRelease(viewport, Qt::Key_Space);
    check(window.doc.cursor == cursor && encodePdf(window.doc.pdf()) == original,
          "Space cancels in-progress signature drag without committing");
    for (auto type : {QEvent::FocusOut, QEvent::WindowDeactivate, QEvent::Hide})
    {
        focus(window);
        QTest::keyPress(viewport, Qt::Key_Space);
        start = viewport->rect().center();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
        QEvent event(type);
        QApplication::sendEvent(
            type == QEvent::WindowDeactivate ? static_cast<QObject*>(&window) : viewport, &event);
        const auto position = scroll(window);
        QTest::mouseMove(viewport, start + QPoint(40, 35), 10);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 35));
        check(scroll(window) == position && !canvas->handToolActive(),
              QString("transient drag stops on event %1").arg(int(type)));
        QTest::keyRelease(viewport, Qt::Key_Space);
    }
    action(window, "handReadingTool")->trigger();
    start = viewport->rect().center();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::keyClick(viewport, Qt::Key_Escape);
    const auto position = scroll(window);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start - QPoint(40, 35));
    check(scroll(window) == position && !canvas->handToolActive(), "Esc cancels persistent drag");
    action(window, "handReadingTool")->trigger();
    window.query->setText("English");
    window.query->setFocus();
    QCoreApplication::processEvents();
    check(window.query->hasFocus() && canvas->handToolActive(),
          "persistent hand survives actual widget focus transfer");
    action(window, "selectReadingTool")->trigger();
    focus(window);
    QTest::keyPress(viewport, Qt::Key_Space);
    window.query->setFocus();
    QCoreApplication::processEvents();
    check(!canvas->handToolActive(), "widget focus transfer clears temporary Space state");
    QTest::keyRelease(window.query, Qt::Key_Space);
    action(window, "handReadingTool")->trigger();
    start = viewport->rect().center();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    window.resize(1100, 780);
    QTest::qWait(50);
    const auto resized = scroll(window);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start - QPoint(40, 35));
    check(scroll(window) == resized && canvas->handToolActive(),
          "resize stops the drag but retains explicit tool");
    action(window, "handReadingTool")->trigger();
    window.doc.busy = true;
    window.refresh();
    check(action(window, "handReadingTool")->isEnabled(), "OCR busy still permits viewing");
    const auto busyPosition = scroll(window);
    drag(window, start, start - QPoint(40, 35));
    check(scroll(window) != busyPosition && encodePdf(window.doc.pdf()) == original,
          "panning remains usable in busy state without changing document");
    window.doc.busy = false;
    window.refresh();
    canvas->beginPlacement();
    check(canvas->placing && !canvas->handToolActive() &&
              viewport->cursor().shape() == Qt::CrossCursor,
          "placement explicitly leaves hand mode");
    QTest::keyClick(viewport, Qt::Key_Escape);
    check(encodePdf(window.doc.pdf()) == original && window.doc.cursor == cursor &&
              window.doc.dirty(),
          "all cancelled operations preserve unsaved signature and undo");
    window.undoAction->trigger();
    check(signatures(window.doc.pdf(), 0).isEmpty(), "Undo still removes the one actual edit");
    window.redoAction->trigger();
    window.doc.save(output + "/hand-preserved-signature.pdf");
    Document reopened;
    reopened.open(output + "/hand-preserved-signature.pdf");
    check(signatures(reopened.pdf(), 0).size() == 1, "signature survives save and reopen");
    return {{"signature_undo_save", true},
            {"focus_deactivate_hide", "synthetic Qt events"},
            {"busy_state", "state simulation; full OCR uses existing regression tests"}};
}
} // namespace tatsu
