#include "compact_viewer_tests.h"
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
void settle()
{
    QTest::qWait(60);
}
void ready(Window& window, int page)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 10000),
          "compact viewer renders the actual PDF");
}
void visible(Window& window, QWidget* control)
{
    check(control && control->isVisible() &&
              window.rect().contains(QRect(control->mapTo(&window, QPoint()), control->size())) &&
              control->visibleRegion().contains(control->rect()),
          QString("control is fully inside its visible area: %1 / %2 / %3; window %4x%5; "
                  "control %6,%7 %8x%9; region %10,%11 %12x%13")
              .arg(control ? control->metaObject()->className() : "missing")
              .arg(control ? control->objectName() : "")
              .arg(qobject_cast<QAbstractButton*>(control)
                       ? static_cast<QAbstractButton*>(control)->text()
                       : "")
              .arg(window.width())
              .arg(window.height())
              .arg(control ? control->mapTo(&window, QPoint()).x() : 0)
              .arg(control ? control->mapTo(&window, QPoint()).y() : 0)
              .arg(control ? control->width() : 0)
              .arg(control ? control->height() : 0)
              .arg(control ? control->visibleRegion().boundingRect().x() : 0)
              .arg(control ? control->visibleRegion().boundingRect().y() : 0)
              .arg(control ? control->visibleRegion().boundingRect().width() : 0)
              .arg(control ? control->visibleRegion().boundingRect().height() : 0));
}
void fixedControls(Window& window)
{
    for (auto name : {"documentToolbar", "workToolbar"})
    {
        auto toolbar = window.findChild<QToolBar*>(name);
        for (auto action : toolbar->actions())
            if (!action->isSeparator())
                visible(window, toolbar->widgetForAction(action));
    }
    for (auto name : {"pageControl", "zoomControl"})
        visible(window, window.findChild<QWidget*>(name));
    for (auto button : window.statusBar()->findChildren<QToolButton*>())
        visible(window, button);
}
void openPanel(Window& window, int panel)
{
    if (auto action = window.findChild<QAction*>("organizeAction"); action->isChecked())
        action->trigger();
    switch (panel)
    {
    case 0:
        window.signatureAction->trigger();
        break;
    case 1:
        window.ocrAction->trigger();
        break;
    case 2:
        window.findChild<QAction*>("writingAction")->trigger();
        break;
    case 3:
        window.signatureAction->menu()->actions()[1]->trigger();
        break;
    case 4:
        window.findChild<QAction*>("organizeAction")->trigger();
        break;
    case 5:
        window.findChild<QAction*>("annotationAction")->trigger();
        break;
    }
    settle();
    check(window.panels->currentIndex() == panel && window.properties->isVisible(),
          "all six settings can be opened");
}
QJsonObject settingsControls(Window& window)
{
    auto scroll = qobject_cast<QScrollArea*>(window.panels->currentWidget());
    check(scroll, "settings have an independent scroll area");
    QList<QWidget*> controls;
    for (auto control : scroll->widget()->findChildren<QWidget*>())
        if (control->isVisible() && control->isEnabled() &&
            (qobject_cast<QAbstractButton*>(control) || qobject_cast<QComboBox*>(control) ||
             qobject_cast<QAbstractSpinBox*>(control) || qobject_cast<QPlainTextEdit*>(control) ||
             (qobject_cast<QLineEdit*>(control) &&
              !qobject_cast<QAbstractSpinBox*>(control->parentWidget()))))
            if (control->focusPolicy() != Qt::NoFocus)
                controls.append(control);
    check(!controls.isEmpty(), "settings contain enabled controls");
    int scrolled = 0;
    for (auto control : controls)
    {
        const auto before = scroll->verticalScrollBar()->value();
        control->setFocus(Qt::TabFocusReason);
        settle();
        check(control->hasFocus() || control->isAncestorOf(QApplication::focusWidget()),
              "settings receive keyboard focus");
        visible(window, control);
        if (scroll->verticalScrollBar()->value() != before)
            ++scrolled;
    }
    // Exercise an actual Tab transition between the adjacent command buttons.
    auto commands = scroll->widget()->findChildren<QPushButton*>();
    if (commands.size() > 1)
    {
        commands.front()->setFocus(Qt::TabFocusReason);
        QTest::keyClick(commands.front(), Qt::Key_Tab);
        settle();
        visible(window, QApplication::focusWidget());
    }
    return {{"controls_reached", controls.size()},
            {"focus_scroll_changes", scrolled},
            {"vertical_scroll_maximum", scroll->verticalScrollBar()->maximum()},
            {"horizontal_scroll_maximum", scroll->horizontalScrollBar()->maximum()}};
}
} // namespace
QJsonObject testCompactViewer(const QString& fixtures, const QString& output)
{
    const auto source = fixtures + "/viewer-navigation.pdf";
    const auto hash = fileHash(source);
    Window window;
    window.show();
    window.openFile(source);
    window.activateWindow();
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    QJsonArray cases;
    for (const auto size : {QSize(1024, 720), QSize(960, 540), QSize(800, 480)})
    {
        window.resize(size);
        settle();
        check(window.QWidget::size() == size,
              "settings do not force the window beyond available size");
        for (int panel = 0; panel < 6; ++panel)
        {
            openPanel(window, panel);
            window.grab().save(output + "/compact-latest-layout.png");
            check(window.QWidget::size() == size,
                  "opening settings preserves requested window size");
            fixedControls(window);
            auto details = settingsControls(window);
            details["width"] = size.width();
            details["height"] = size.height();
            details["panel"] = panel;
            cases.append(details);
            check(window.grab().save(output + QString("/compact-%1-%2-%3.png")
                                                  .arg(size.width())
                                                  .arg(size.height())
                                                  .arg(panel)),
                  "save compact layout image");
        }
    }
    openPanel(window, 0);
    ready(window, 0);
    auto anchor = window.canvas->anchor();
    auto scroll = qobject_cast<QScrollArea*>(window.panels->currentWidget());
    scroll->verticalScrollBar()->setValue(0);
    settle();
    const auto after = window.canvas->anchor();
    check(anchor.page == after.page && QLineF(anchor.point, after.point).length() <= .01,
          "scrolling settings does not move the document");
    check(window.doc.cursor == 0 && !window.doc.dirty() && encodePdf(window.doc.pdf()) == original,
          "layout and focus never modify PDF or Undo history");

    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    settle();
    visible(window, window.query);
    window.query->setText("English");
    auto results = window.findChild<SearchSession*>();
    QTest::keyClick(window.query, Qt::Key_Return);
    check(QTest::qWaitFor([&] { return results->complete() && !results->matches().isEmpty(); },
                          10000),
          "search remains usable with compact signature settings");
    QTest::keyClick(window.query, Qt::Key_Escape);
    QTest::keyClick(window.canvas->viewport(), Qt::Key_L, Qt::ControlModifier);
    auto number = window.findChild<QLineEdit*>("pageNumber");
    visible(window, number);
    number->setText("3");
    QTest::keyClick(number, Qt::Key_Return);
    ready(window, 2);
    check(window.canvas->page == 2, "compact page input navigates real PDF");

    openPanel(window, 0);
    window.canvas->fitPage();
    ready(window, 2);
    window.signature->setPlainText("山田 太郎");
    auto place = window.findChild<QPushButton*>("placeSignature");
    place->setFocus(Qt::TabFocusReason);
    settle();
    visible(window, place);
    QTest::keyClick(place, Qt::Key_Space);
    check(window.canvas->placing, "keyboard starts signature placement");
    const auto point = window.canvas->pdfToViewport(2, {140, 300}).toPoint();
    check(window.canvas->viewport()->rect().contains(point), "signature point is visible");
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    check(signatures(window.doc.pdf(), 2).size() == 1 && window.doc.cursor == 1,
          "compact viewer places a Japanese signature");
    const auto placed = signatures(window.doc.pdf(), 2).front();
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Z, Qt::ControlModifier);
    check(signatures(window.doc.pdf(), 2).isEmpty() && !window.doc.dirty(),
          "keyboard Undo restores the PDF");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(signatures(window.doc.pdf(), 2).size() == 1, "keyboard Redo restores the signature");
    const auto saved = output + "/compact-signature.pdf";
    window.doc.save(saved);
    window.openFile(saved);
    ready(window, 0);
    const auto reopened = signatures(window.doc.pdf(), 2).front();
    const auto savedPositionError =
        qMax(QLineF(reopened.rect.topLeft(), placed.rect.topLeft()).length(),
             QLineF(reopened.rect.bottomRight(), placed.rect.bottomRight()).length());
    check(reopened.text == "山田 太郎" && savedPositionError <= .5,
          "saved Japanese signature retains text and position");
    // Opening protects the source by asking for a destination on the first save.
    // Establish this owned test PDF as the target before exercising Ctrl+S.
    window.doc.save(saved);
    window.doc.moveSignature(2, reopened, {20, 10});
    window.refresh();
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_S, Qt::ControlModifier);
    Document verified;
    verified.open(saved);
    check(!window.doc.dirty() && QLineF(signatures(verified.pdf(), 2).front().rect.topLeft(),
                                        reopened.rect.topLeft() + QPointF(20, 10))
                                         .length() <= .5,
          "compact keyboard Save persists reediting");
    check(fileHash(source) == hash, "original remains unchanged");
    return {{"layouts", cases},
            {"signature_save_reedit", true},
            {"saved_position_error_pt", savedPositionError},
            {"coordinate_limit_pt", .5},
            {"native_OS_DPI", "未実行"}};
}
QJsonObject testCompactOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(800, 480);
    window.show();
    window.openFile(fixtures + "/D03.pdf");
    window.doc.putSignature(0, "OCR中も保持", {50, 40}, 16, Qt::black);
    window.refresh();
    openPanel(window, 1);
    const auto before = window.doc.pdf();
    const auto revision = window.doc.revision;
    window.startOcr();
    check(
        QTest::qWaitFor(
            [&] { return window.worker && window.progress->text().startsWith("OCR 1 /"); }, 90000),
        "actual OCR reaches progress in compact window");
    visible(window, window.cancel);
    fixedControls(window);
    // Long explanations must not steal the fixed navigation or cancellation area.
    const QString explanation(600, QChar(0x4e00));
    window.status->setText(explanation);
    window.progress->setText(explanation);
    settle();
    check(window.status->text() == explanation && window.progress->text() == explanation,
          "full status text is retained");
    visible(window, window.cancel);
    fixedControls(window);
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_L, Qt::ControlModifier);
    auto number = window.findChild<QLineEdit*>("pageNumber");
    number->setText("2");
    QTest::keyClick(number, Qt::Key_Return);
    ready(window, 1);
    window.grab().save(output + "/compact-ocr-running.png");
    window.cancel->setFocus();
    QTest::keyClick(window.cancel, Qt::Key_Space);
    check(QTest::qWaitFor([&] { return !window.worker; }, 10000),
          "keyboard cancellation completes");
    check(window.doc.pdf() == before && window.doc.revision == revision && window.doc.dirty() &&
              window.QWidget::size() == QSize(800, 480),
          "compact OCR cancellation preserves the unsaved PDF");
    return {{"logical_size", "800x480"},
            {"OCR_cancel_preserves_changes", true},
            {"native_keyboard_and_IME", "未実行; QtTest events"}};
}
} // namespace tatsu
