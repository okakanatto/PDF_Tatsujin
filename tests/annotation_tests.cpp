#include "annotation_tests.h"
#include "annotation_operations.h"
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
Signature mark(const Document& document, OverlayKind kind)
{
    for (const auto& item : signatures(document.pdf(), 0))
        if (item.kind == kind)
            return item;
    fail("expected annotation missing");
}
void ready(Window& window)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 10000),
          "actual annotation PDF ready");
}
QPointF endpoint(Window& window, const QString& phrase, bool end)
{
    for (const auto& flow : PDFTextFlow::createTextFlows(textLayout(window.doc.pdf(), 0),
                                                         PDFTextFlow::AddLineBreaks, 0))
    {
        auto index = flow.getText().indexOf(phrase);
        if (index >= 0)
        {
            auto box = flow.getBoundingBoxes()[size_t(index + (end ? phrase.size() - 1 : 0))];
            return {end ? box.right() + .6 : box.left() - .6, box.center().y()};
        }
    }
    fail("frozen highlight phrase missing");
}
void drag(Window& window, QPointF from, QPointF to)
{
    auto viewport = window.canvas->viewport();
    const auto start = window.canvas->pdfToViewport(0, from).toPoint();
    const auto finish = window.canvas->pdfToViewport(0, to).toPoint();
    check(viewport->rect().contains(start) && viewport->rect().contains(finish),
          "annotation drag endpoints visible");
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(viewport, finish, 20);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, finish);
}
} // namespace
QJsonObject testAnnotations(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D01.pdf");
    const auto sourceHash = fileHash(document.source);
    const auto body = pageText(document.pdf(), 0);
    const auto plain = renderPage(document.pdf(), 0, 1.5);
    putAnnotation(document, 0, OverlayKind::Comment, {60, 300, 24, 24}, "日本語の確認コメント",
                  Qt::red, 1.5);
    putAnnotation(document, 0, OverlayKind::Rectangle, {110, 300, 100, 50}, "枠", Qt::blue, 2);
    putAnnotation(document, 0, OverlayKind::Line, {240, 280, 100, 70}, "直線", Qt::green, 2,
                  QPolygonF{QPointF(250, 290), QPointF(330, 340)});
    putAnnotation(document, 0, OverlayKind::Arrow, {350, 280, 130, 90}, "矢印", Qt::red, 2,
                  QPolygonF{QPointF(365, 295), QPointF(465, 350)});
    const auto beforeHighlight = document.pdf();
    putHighlights(document, {{0, {{60, 440, 140, 15}, {60, 465, 120, 15}}}}, Qt::yellow);
    document.undo();
    check(document.pdf() == beforeHighlight, "multi-line highlight is one Undo unit");
    document.redo();
    check(signatures(document.pdf(), 0).size() == 5, "all five annotation types created");
    auto arrow = mark(document, OverlayKind::Arrow);
    document.moveSignature(0, arrow, {10, 20});
    auto moved = mark(document, OverlayKind::Arrow);
    check(moved.geometry ==
              QPolygonF{arrow.geometry[0] + QPointF(10, 20), arrow.geometry[1] + QPointF(10, 20)},
          "moving arrow translates both endpoints");
    document.undo();
    check(mark(document, OverlayKind::Arrow).geometry == arrow.geometry,
          "Undo restores arrow geometry");
    document.redo();
    {
        QFile diagnostic(output + "/annotations-before-validation.pdf");
        check(diagnostic.open(QIODevice::WriteOnly), "diagnostic output");
        diagnostic.write(encodePdf(document.pdf()));
    }
    document.save(output + "/annotations.pdf");
    const auto visual = renderPage(document.pdf(), 0, 1.5);
    const auto interior =
        pageMatrix(document.pdf().getCatalog()->getPage(0), 1.5).map(QPointF(160, 325)).toPoint();
    check(visual.pixelColor(interior) == plain.pixelColor(interior),
          "rectangle interior remains transparent and never obscures body");
    visual.save(output + "/annotations.png");
    Document reopened;
    reopened.open(output + "/annotations.pdf");
    check(renderPage(reopened.pdf(), 0, 1.5) == visual, "saved annotations render identically");
    check(pageText(reopened.pdf(), 0) == body, "annotation keeps original body text");
    const QStringList subtypes{"Text", "Square", "Line", "Line", "Highlight"};
    const auto items = signatures(reopened.pdf(), 0);
    for (int i = 0; i < items.size(); ++i)
    {
        const auto object = reopened.pdf().getObjectByReference(items[i].ref);
        const auto dictionary = object.getDictionary();
        check(dictionary->get("Subtype").getString() == subtypes[i].toLatin1(),
              "annotation uses standard PDF subtype");
        check(dictionary->hasKey("AP") && dictionary->get("F").getInteger() == 4,
              "annotation has appearance and print flag");
    }
    check(mark(reopened, OverlayKind::Comment).text == "日本語の確認コメント",
          "Japanese comment contents reeditable");
    auto rectangle = mark(reopened, OverlayKind::Rectangle);
    putAnnotation(reopened, 0, rectangle.kind, {110, 300, 130, 65}, "更新した矩形", Qt::red, 3, {},
                  rectangle.ref);
    auto highlight = mark(reopened, OverlayKind::Highlight);
    reopened.moveSignature(0, highlight, {3, 4});
    auto shifted = mark(reopened, OverlayKind::Highlight);
    check(shifted.geometry[0] == highlight.geometry[0] + QPointF(3, 4),
          "highlight moves actual quadrilaterals");
    putAnnotation(reopened, 0, shifted.kind, shifted.rect, "変更済み", Qt::cyan, 1,
                  shifted.geometry, shifted.ref);
    reopened.eraseSignature(0, mark(reopened, OverlayKind::Line));
    reopened.undo();
    check(signatures(reopened.pdf(), 0).size() == 5, "annotation deletion Undo");
    reopened.redo();
    reopened.save(output + "/annotations-reedited.pdf");
    Document all;
    all.open(output + "/annotations.pdf");
    const auto originals = signatures(all.pdf(), 0);
    for (const auto& item : originals)
    {
        auto geometry = item.geometry;
        for (auto& point : geometry)
            point += QPointF(2, 3);
        putAnnotation(all, 0, item.kind, item.rect.translated(2, 3), "全種類の更新を確認",
                      Qt::magenta, 2, geometry, item.ref);
        check(mark(all, item.kind).text == "全種類の更新を確認", "each annotation kind editable");
    }
    all.save(output + "/annotations-all-edited.pdf");
    for (const auto& item : signatures(all.pdf(), 0))
        all.eraseSignature(0, item);
    const auto pageObject =
        all.pdf().getObjectByReference(all.pdf().getCatalog()->getPage(0)->getPageReference());
    const auto remaining = all.pdf().getObject(pageObject.getDictionary()->get("Annots"));
    check(signatures(all.pdf(), 0).isEmpty() &&
              (!remaining.isArray() || remaining.getArray()->getCount() == 0),
          "all five deleted; no orphan comment popups remain");
    all.undo();
    check(signatures(all.pdf(), 0).size() == 1, "Undo restores only last deleted annotation");
    all.redo();
    all.save(output + "/annotations-all-deleted.pdf");
    check(fileHash(document.source) == sourceHash, "annotations protect original");
    return {{"types", 5},
            {"standard_AP", true},
            {"move_geometry", true},
            {"save_reedit_delete", true},
            {"original_unchanged", true}};
}
QJsonObject testAnnotationInput(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(1024, 720);
    window.show();
    window.openFile(fixtures + "/viewer-search.pdf");
    window.canvas->setZoom(.5);
    ready(window);
    window.findChild<QAction*>("annotationAction")->trigger();
    auto panel = window.findChild<QWidget*>("annotationPanel");
    auto kind = panel->findChild<QComboBox*>("annotationKind");
    auto place = panel->findChild<QPushButton*>("placeAnnotation");
    auto contents = panel->findChild<QPlainTextEdit*>("annotationContents");
    const QList<QPair<QPointF, QPointF>> locations{
        {{80, 180}, {180, 240}}, {{240, 180}, {330, 240}}, {{370, 180}, {460, 240}}};
    for (int i = 0; i < 3; ++i)
    {
        kind->setCurrentIndex(i + 1);
        contents->setPlainText("ドラッグで作成");
        const auto cursor = window.doc.cursor;
        place->click();
        drag(window, locations[i].first, locations[i].second);
        check(window.doc.cursor == cursor + 1, "shape drag creates exactly one Undo unit");
        window.undoAction->trigger();
        check(signatures(window.doc.pdf(), 0).size() == i, "shape Undo removes only newest");
        window.redoAction->trigger();
        ready(window);
    }
    const auto beforeCancel = window.doc.pdf();
    place->click();
    auto viewport = window.canvas->viewport();
    auto start = window.canvas->pdfToViewport(0, {200, 300}).toPoint();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(viewport, start + QPoint(40, 30));
    QTest::keyClick(viewport, Qt::Key_Escape);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 30));
    check(window.doc.pdf() == beforeCancel, "Esc cancels shape without partial commit");
    const QString phrase = "交通費を申請します。";
    drag(window, endpoint(window, phrase, false), endpoint(window, phrase, true));
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "selected real text ready");
    check(window.canvas->copied == phrase, "highlight starts from exact frozen Japanese text");
    kind->setCurrentIndex(4);
    place->click();
    check(mark(window.doc, OverlayKind::Highlight).geometry.size() >= 4,
          "UI creates actual text quadrilaterals");
    ready(window);
    drag(window, endpoint(window, phrase, false), endpoint(window, phrase, true));
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "highlighted text selection ready");
    QTest::keyClick(viewport, Qt::Key_C, Qt::ControlModifier);
    check(QApplication::clipboard()->text() == phrase, "body copy remains usable under highlight");
    window.doc.save(output + "/annotations-ui.pdf");
    ready(window);
    window.grab().save(output + "/annotations-ui.png");
    window.openFile(output + "/annotations-ui.pdf");
    ready(window);
    window.findChild<QAction*>("annotationAction")->trigger();
    auto list = panel->findChild<QListWidget*>("ownedAnnotationList");
    list->setCurrentRow(0);
    contents->setPlainText("保存後の注釈編集");
    panel->findChild<QPushButton*>("updateAnnotation")->click();
    check(mark(window.doc, OverlayKind::Rectangle).text == "保存後の注釈編集",
          "saved shape editable through actual panel");
    panel->findChild<QPushButton*>("removeAnnotation")->click();
    check(signatures(window.doc.pdf(), 0).size() == 3, "UI annotation deletion");
    return {{"pointer_shapes", 3},
            {"text_highlight", true},
            {"copy", true},
            {"cancel_atomic", true},
            {"save_reedit", true}};
}
} // namespace tatsu
