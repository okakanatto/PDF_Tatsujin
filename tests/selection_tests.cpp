#include "selection_tests.h"
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
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 10000), "PDF is visible");
}
void finished(Window& window)
{
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "selection text is ready");
}
QPointF endpoint(Window& window, int page, const QString& phrase, bool ending)
{
    const auto layout = textLayout(window.doc.pdf(), page);
    for (const auto& flow : PDFTextFlow::createTextFlows(layout, PDFTextFlow::AddLineBreaks, page))
    {
        auto at = flow.getText().indexOf(phrase);
        if (at < 0)
            continue;
        const auto boxes = flow.getBoundingBoxes();
        const auto box = boxes[size_t(at + (ending ? phrase.size() - 1 : 0))];
        return {ending ? box.right() + .6 : box.left() - .6, box.center().y()};
    }
    fail("Fixture does not contain frozen phrase: " + phrase);
}
void drag(Window& window, int fromPage, QPointF from, int toPage, QPointF to)
{
    auto viewport = window.canvas->viewport();
    auto a = window.canvas->pdfToViewport(fromPage, from).toPoint();
    auto b = window.canvas->pdfToViewport(toPage, to).toPoint();
    check(viewport->rect().contains(a) && viewport->rect().contains(b),
          "both drag endpoints are visible");
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, a);
    QTest::mouseMove(viewport, b, 20);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, b);
    finished(window);
}
QString copy(Window& window)
{
    QApplication::clipboard()->setText("clipboard sentinel");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_C, Qt::ControlModifier);
    return QApplication::clipboard()->text();
}
QJsonObject expectations(const QString& fixtures)
{
    QFile file(fixtures + "/viewer-selection-manifest.json");
    check(file.open(QIODevice::ReadOnly), "frozen copy expectations");
    return QJsonDocument::fromJson(file.readAll()).object()["expected"].toObject();
}
} // namespace
QJsonObject testSelectionRanges(const QString& fixtures, const QString& output)
{
    Window w;
    w.show();
    w.openFile(fixtures + "/viewer-search.pdf");
    w.canvas->setZoom(.4);
    ready(w, 1);
    const auto expected = expectations(fixtures);
    const auto original = encodePdf(w.doc.pdf());
    auto from = endpoint(w, 0, "交通費を申請します。", false);
    auto lineEnd = endpoint(w, 0, "交通費を申請します。", true);
    drag(w, 0, from, 0, lineEnd);
    check(copy(w) == expected["japanese_line"].toString(),
          "exact Japanese line copy: " + w.canvas->copied);
    auto to = endpoint(w, 1, "Travel notes: alpha closes this section.", true);
    drag(w, 0, from, 1, to);
    check(copy(w) == expected["cross_page_forward_and_reverse"].toString(),
          "exact cross-page forward copy: " + w.canvas->copied);
    w.grab().save(output + "/selection-cross-page.png");
    const auto forward = w.canvas->copied;
    drag(w, 1, to, 0, from);
    check(copy(w) == forward, "reverse selection gives the same text and page order");
    w.canvas->setZoom(1.1);
    check(copy(w) == forward, "completed selection survives zoom");
    w.grab().save(output + "/selection-readable.png");
    bool menuUsed = false;
    QApplication::clipboard()->setText("context sentinel");
    QTimer::singleShot(0, &w,
                       [&]
                       {
                           if (auto menu = w.canvas->findChild<QMenu*>("textSelectionMenu"))
                           {
                               auto action = menu->findChild<QAction*>("copySelectedText");
                               if (action && action->isEnabled())
                               {
                                   action->trigger();
                                   menuUsed = true;
                               }
                               menu->close();
                           }
                       });
    const auto position = w.canvas->pdfToViewport(0, from).toPoint();
    QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, position,
                                w.canvas->viewport()->mapToGlobal(position));
    QCoreApplication::sendEvent(w.canvas->viewport(), &menuEvent);
    check(menuUsed && QApplication::clipboard()->text() == forward, "context copy equals Ctrl+C");
    QTest::keyClick(w.canvas->viewport(), Qt::Key_Escape);
    check(copy(w) == "clipboard sentinel", "no selection leaves clipboard untouched");
    check(w.canvas->copied.isEmpty(), "Escape clears selection");
    check(!w.doc.dirty() && w.doc.cursor == 0 && encodePdf(w.doc.pdf()) == original,
          "selection leaves PDF, dirty and Undo unchanged");
    return {{"forward_and_reverse_exact", true},
            {"Japanese_exact", true},
            {"context_copy", true},
            {"characters", forward.size()},
            {"cache_bytes", w.canvas->selectionCacheBytes()}};
}
QJsonObject testSelectionScroll(const QString& fixtures, const QString& output)
{
    Window w;
    w.show();
    w.openFile(fixtures + "/viewer-search.pdf");
    w.canvas->setZoom(.6);
    ready(w);
    const auto start =
        w.canvas->pdfToViewport(0, endpoint(w, 0, "ALPHA opens this paragraph.", false)).toPoint();
    auto viewport = w.canvas->viewport();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    int ticks = 0;
    QTimer pulse;
    QObject::connect(&pulse, &QTimer::timeout, [&] { ++ticks; });
    pulse.start(5);
    const QPoint edge(viewport->width() / 2, viewport->height() - 2);
    QTest::mouseMove(viewport, edge, 20);
    check(QTest::qWaitFor([&] { return w.canvas->visiblePages().contains(2); }, 5000),
          "edge drag scrolls to the third page");
    check(QTest::qWaitFor(
              [&]
              {
                  const auto target = w.canvas->pdfToViewport(2, {300, 190});
                  return viewport->rect().adjusted(0, 32, 0, -32).contains(target.toPoint());
              },
              5000),
          "target text enters the viewport during autoscroll");
    auto end = w.canvas->pdfToViewport(2, endpoint(w, 2, "交通費の最終確認です。", true)).toPoint();
    QTest::mouseMove(viewport, end, 10);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
    finished(w);
    pulse.stop();
    check(copy(w) == expectations(fixtures)["three_pages"].toString(),
          "exact three-page copy after scrolling: " + w.canvas->copied);
    const auto scroll = w.canvas->verticalScrollBar()->value();
    QTest::qWait(80);
    check(w.canvas->verticalScrollBar()->value() == scroll, "release stops autoscroll");
    check(ticks > 0, "UI timer continues during selection and autoscroll");
    w.grab().save(output + "/selection-autoscroll.png");
    w.openFile(fixtures + "/D10-digital-100.pdf");
    w.canvas->setZoom(.7);
    ready(w);
    QTest::qWait(100);
    check(w.canvas->selectionExtractedPages() < 10,
          "opening 100 pages does not synchronously extract all text");
    const int initiallyExtracted = w.canvas->selectionExtractedPages();
    const auto beginning = w.canvas->pdfToViewport(0, endpoint(w, 0, "日本語", false)).toPoint();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, beginning);
    w.canvas->verticalScrollBar()->setValue(w.canvas->verticalScrollBar()->maximum());
    ready(w, 99);
    auto finish = w.canvas->pdfToViewport(99, endpoint(w, 99, "確認します。", true)).toPoint();
    QElapsedTimer timer;
    timer.start();
    ticks = 0;
    pulse.start(5);
    QTest::mouseMove(viewport, finish);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, finish);
    if (!w.canvas->selectionReady())
        check(copy(w) == "clipboard sentinel", "pending selection never publishes partial text");
    finished(w);
    const auto elapsed = timer.elapsed();
    pulse.stop();
    check(w.canvas->copied.count("日本語とEnglishの文書です。") == 100 &&
              w.canvas->copied.count("PDFの検索とコピー、署名の保存を確認します。") == 100,
          "100-page selection contains each frozen paragraph exactly once");
    check(ticks > 0, "UI timer continues during 100-page text loading");
    const int largeTicks = ticks;
    const auto largeBytes = w.canvas->selectionCacheBytes();
    w.canvas->goToPage(0);
    ready(w);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier,
                      w.canvas->pdfToViewport(0, endpoint(w, 0, "日本語", false)).toPoint());
    QTest::mouseMove(viewport, edge, 20);
    QTest::keyClick(viewport, Qt::Key_Escape);
    const auto stopped = w.canvas->verticalScrollBar()->value();
    QTest::qWait(80);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, edge);
    check(w.canvas->verticalScrollBar()->value() == stopped && w.canvas->copied.isEmpty(),
          "Escape stops active autoscroll and mouse release does not restore selection");
    return {{"three_pages_exact", true},     {"D10_extracted_pages_at_open", initiallyExtracted},
            {"D10_selection_ms", elapsed},   {"D10_UI_timer_ticks", largeTicks},
            {"D10_cache_bytes", largeBytes}, {"Escape_stops_autoscroll", true}};
}
QJsonObject testSelectionLifecycle(const QString& fixtures, const QString& output)
{
    Q_UNUSED(output);
    Window w;
    w.show();
    w.openFile(fixtures + "/D02.pdf");
    for (int number = 0; number < 3; ++number)
    {
        w.canvas->goToPage(number);
        w.canvas->fitPage();
        ready(w, number);
        drag(w, number, endpoint(w, number, "PDF", false), number,
             endpoint(w, number, "PDF", true));
        check(copy(w) == "PDF", "exact copy on rotated CropBox/UserUnit page");
    }
    w.canvas->goToPage(3);
    w.canvas->fitPage();
    ready(w, 3);
    const auto crop = w.doc.pdf().getCatalog()->getPage(3)->getCropBox();
    drag(w, 3, crop.center() - QPointF(15, 10), 3, crop.center() + QPointF(15, 10));
    check(copy(w) == "clipboard sentinel" && w.canvas->copied.isEmpty(),
          "fully cropped text cannot be copied");
    check(w.canvas->selectionMessage().contains("OCR"),
          "textless page offers OCR without fake selection");
    w.openFile(fixtures + "/viewer-search.pdf");
    w.canvas->setZoom(.4);
    ready(w);
    const auto from = endpoint(w, 0, "ALPHA", false), to = endpoint(w, 0, "ALPHA", true);
    drag(w, 0, from, 0, to);
    check(copy(w) == "ALPHA", "selection in replacement document: " + w.canvas->copied);
    w.doc.putSignature(0, "山田 太郎", {100, 200}, 14, Qt::black);
    w.refresh();
    check(copy(w) == "clipboard sentinel", "document revision clears old selection");
    w.undoAction->trigger();
    check(w.canvas->copied.isEmpty(), "Undo does not revive selection from a previous revision");
    w.doc.open(fixtures + "/viewer-selection-restricted.pdf", "selection-user");
    w.refresh(true);
    check(!w.doc.copyAllowed, "encrypted fixture forbids copy");
    check(copy(w) == "clipboard sentinel",
          "copy permission is enforced without clearing clipboard");
    return {{"rotation_crop_userunit", true},
            {"revision_clears_selection", true},
            {"copy_permission", true},
            {"OS_clipboard", "未実行; Qt offscreen clipboard"}};
}
QJsonObject testSelectionOcr(const QString& pdfPath, const QString& output)
{
    Window w;
    w.show();
    w.openFile(pdfPath);
    w.canvas->setZoom(.4);
    ready(w);
    auto viewport = w.canvas->viewport();
    const auto firstCrop = w.doc.pdf().getCatalog()->getPage(0)->getCropBox();
    const auto lastCrop = w.doc.pdf().getCatalog()->getPage(w.doc.pages() - 1)->getCropBox();
    const auto start =
        w.canvas->pdfToViewport(0, firstCrop.bottomLeft() + QPointF(1, -1)).toPoint();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    w.canvas->verticalScrollBar()->setValue(w.canvas->verticalScrollBar()->maximum());
    ready(w, w.doc.pages() - 1);
    const auto end =
        w.canvas->pdfToViewport(w.doc.pages() - 1, lastCrop.topRight() + QPointF(-1, 1)).toPoint();
    QTest::mouseMove(viewport, end, 20);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
    finished(w);
    const auto value = copy(w);
    QStringList expected;
    for (int i = 0; i < w.doc.pages(); ++i)
    {
        auto text = pageText(w.doc.pdf(), i).replace("\r\n", "\n");
        while (text.endsWith('\n'))
            text.chop(1);
        expected.append(text);
    }
    check(value == expected.join("\n\n"),
          "selection copies every OCR page once, in document order");
    QFile file(output + "/D03-selection-copy.json");
    check(file.open(QIODevice::WriteOnly), "write actual OCR clipboard evidence");
    file.write(QJsonDocument(QJsonArray::fromStringList(value.split("\n\n"))).toJson());
    w.grab().save(output + "/selection-ocr.png");
    return {{"pages", w.doc.pages()},
            {"clipboard_characters", value.size()},
            {"full_text_order_matches", true},
            {"accuracy_judgment", "external frozen-truth evaluator required"}};
}
} // namespace tatsu
