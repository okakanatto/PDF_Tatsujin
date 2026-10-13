#include "selection_tests.h"
#include "selection_text.h"
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
namespace
{
QPointF glyph(Window& window, int page, const QString& phrase, qsizetype offset = 0)
{
    for (const auto& flow : PDFTextFlow::createTextFlows(textLayout(window.doc.pdf(), page),
                                                         PDFTextFlow::AddLineBreaks, page))
    {
        const auto at = flow.getText().indexOf(phrase);
        if (at >= 0)
        {
            const auto index = at + offset;
            const auto boxes = flow.getBoundingBoxes();
            if (boxes[size_t(index)].isValid())
                return boxes[size_t(index)].center();
            if (flow.getText()[index].isSpace() && index > 0 && index + 1 < flow.getText().size())
                return {(boxes[size_t(index - 1)].right() + boxes[size_t(index + 1)].left()) / 2,
                        boxes[size_t(index - 1)].center().y()};
            fail("Fixture token has no visible glyph: " + phrase);
        }
    }
    fail("Missing frozen word: " + phrase);
}
void wordPress(Window& window, int page, QPointF point)
{
    const auto position = window.canvas->pdfToViewport(page, point).toPoint();
    check(window.canvas->viewport()->rect().contains(position), "word is visible");
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    QTest::mouseDClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, position);
}
void wordClick(Window& window, int page, QPointF point)
{
    wordPress(window, page, point);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                        window.canvas->pdfToViewport(page, point).toPoint());
    finished(window);
}
void hover(Window& window, QPoint position)
{
    // Offscreen does not have an OS window under QTest's global cursor movement.
    QMouseEvent move(QEvent::MouseMove, position, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.canvas->viewport(), &move);
}
} // namespace
QJsonObject testSelectionWords(const QString& fixtures, const QString& output)
{
    Window w;
    w.show();
    w.openFile(fixtures + "/viewer-search.pdf");
    w.canvas->setZoom(.4);
    ready(w, 1);
    const auto original = encodePdf(w.doc.pdf());
    for (const auto& token : {QString("opens"), QString("paragraph"), QString("."), QString(" ")})
    {
        const auto point = token == "."   ? glyph(w, 0, "paragraph.", 9)
                           : token == " " ? glyph(w, 0, "opens this", 5)
                                          : glyph(w, 0, token);
        wordClick(w, 0, point);
        check(copy(w) == token,
              "double click exact token: expected [" + token + "] got [" + w.canvas->copied + "]");
    }
    wordClick(w, 0, glyph(w, 0, "交通費"));
    check(copy(w) == "交", "Japanese Unicode word boundary, with no invented phrase grouping");
    const auto begin = glyph(w, 0, "opens"), end = glyph(w, 0, "paragraph");
    for (const bool reverse : {false, true})
    {
        wordPress(w, 0, reverse ? end : begin);
        const auto finish = w.canvas->pdfToViewport(0, reverse ? begin : end).toPoint();
        QTest::mouseMove(w.canvas->viewport(), finish, 20);
        QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, finish);
        finished(w);
        check(copy(w) == "opens this paragraph", "word drag is exact in both directions");
    }
    wordClick(w, 0, glyph(w, 0, "ALPHA"));
    QTest::mouseClick(w.canvas->viewport(), Qt::LeftButton, Qt::ShiftModifier,
                      w.canvas->pdfToViewport(0, end).toPoint());
    finished(w);
    check(copy(w) == "ALPHA opens this paragraph", "Shift click keeps the original word anchor");
    const auto across = glyph(w, 1, "Travel");
    wordPress(w, 0, begin);
    QTest::mouseMove(w.canvas->viewport(), w.canvas->pdfToViewport(1, across).toPoint(), 20);
    QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                        w.canvas->pdfToViewport(1, across).toPoint());
    finished(w);
    const QString expected =
        "opens this paragraph.\nMiddle context: alpha is the second occurrence.\n"
        "交通費を申請します。\n交通費の領収書を確認します。\n\n"
        "Before the destination\nTravel";
    check(copy(w) == expected,
          "word drag crosses a page with exact separators: " + w.canvas->copied);
    w.grab().save(output + "/selection-words-cross-page.png");
    wordPress(w, 1, across);
    QTest::mouseMove(w.canvas->viewport(), w.canvas->pdfToViewport(0, begin).toPoint(), 20);
    QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                        w.canvas->pdfToViewport(0, begin).toPoint());
    finished(w);
    check(copy(w) == expected, "reverse word drag crosses a page identically");
    QTest::keyClick(w.canvas->viewport(), Qt::Key_Escape);
    auto from = endpoint(w, 0, "ALPHA", false);
    auto to = endpoint(w, 0, "ALPHA", true);
    drag(w, 0, from, 0, to);
    QTest::mouseClick(w.canvas->viewport(), Qt::LeftButton, Qt::ShiftModifier,
                      w.canvas->pdfToViewport(0, endpoint(w, 0, "opens this", true)).toPoint());
    finished(w);
    check(copy(w) == "ALPHA opens this", "Shift click extends a character selection");
    check(!w.doc.dirty() && w.doc.cursor == 0 && encodePdf(w.doc.pdf()) == original,
          "word selection leaves document and Undo untouched");
    return {{"exact_tokens", 5},
            {"word_drag_both_directions", true},
            {"cross_page_both_directions", true},
            {"Shift_word_and_character", true},
            {"PDF_and_Undo_unchanged", true}};
}
QJsonObject testSelectionWordBoundaries(const QString&, const QString&)
{
    SelectionPage page;
    page.text = QString::fromUtf8("can't 42 é 👩‍💻\n交");
    const auto lineBreak = page.text.indexOf('\n');
    for (qsizetype i = 0; i < page.text.size(); ++i)
        page.boxes.append(i == lineBreak  ? QRectF()
                          : i < lineBreak ? QRectF(i * 10, 0, 10, 10)
                                          : QRectF((i - lineBreak - 1) * 10, 20, 10, 10));
    page.lines = {{0, lineBreak, QRectF(0, 0, lineBreak * 10, 10)},
                  {lineBreak + 1, page.text.size(), QRectF(0, 20, 10, 10)}};
    page.indexBoundaries();
    int checks = 0;
    for (const auto& token : {QString("can't"), QString("42"), QString::fromUtf8("é"),
                              QString::fromUtf8("👩‍💻"), QString("交")})
    {
        const auto at = page.text.indexOf(token);
        for (qsizetype i = at; i < at + token.size(); ++i)
        {
            const auto span = page.wordAt(page.boxes[i].center());
            check(span.first >= 0 && page.text.mid(span.first, span.second - span.first) == token,
                  "Unicode word keeps every UTF-16 unit of [" + token + "] together");
            const auto caret = page.caret(page.boxes[i].center() - QPointF(1, 0));
            check(page.graphemes.contains(caret), "caret never splits a Unicode grapheme");
            ++checks;
        }
    }
    check(page.wordAt({200, 100}).first == -1 && !page.contains({200, 100}),
          "blank margins have neither a word nor a text hit");
    return {{"UTF16_positions_checked", checks},
            {"combining_and_emoji_unsplit", true},
            {"apostrophe_and_numbers", true},
            {"blank_margin", true}};
}
QJsonObject testSelectionWordLifecycle(const QString& fixtures, const QString& output)
{
    Window w;
    w.show();
    w.openFile(fixtures + "/D02.pdf");
    for (int number = 0; number < 3; ++number)
    {
        w.canvas->goToPage(number);
        w.canvas->fitPage();
        ready(w, number);
        wordClick(w, number, glyph(w, number, "PDF"));
        check(copy(w) == "PDF", "word on rotated CropBox/UserUnit page");
    }
    w.openFile(fixtures + "/viewer-search.pdf");
    w.canvas->setZoom(.4);
    ready(w);
    const auto point = glyph(w, 0, "opens");
    const auto position = w.canvas->pdfToViewport(0, point).toPoint();
    hover(w, position);
    const bool textCursor = QTest::qWaitFor(
        [&] { return w.canvas->viewport()->cursor().shape() == Qt::IBeamCursor; }, 10000);
    if (!textCursor)
    {
        w.grab().save(output + "/word-hover-failure.png");
        const auto after = w.canvas->pdfToViewport(0, point);
        fail(QString("text hover uses I-beam after async extraction; cursor=%1 pages=%2 "
                     "position=(%3,%4) after=(%5,%6) viewport=(%7,%8)")
                 .arg(w.canvas->viewport()->cursor().shape())
                 .arg(w.canvas->selectionExtractedPages())
                 .arg(position.x())
                 .arg(position.y())
                 .arg(after.x())
                 .arg(after.y())
                 .arg(w.canvas->viewport()->width())
                 .arg(w.canvas->viewport()->height()));
    }
    const auto blank = w.doc.pdf().getCatalog()->getPage(0)->getCropBox().topLeft() + QPointF(5, 5);
    wordClick(w, 0, blank);
    check(copy(w) == "clipboard sentinel" && w.canvas->copied.isEmpty(),
          "blank double click does not choose the nearest word");
    hover(w, w.canvas->pdfToViewport(0, blank).toPoint());
    check(w.canvas->viewport()->cursor().shape() == Qt::ArrowCursor, "blank uses arrow");
    wordClick(w, 0, point);
    check(copy(w) == "opens", "selection before revision change");
    const auto s = w.doc.putSignature(0, "山田 太郎", point - QPointF(3, 3), 20, Qt::black);
    w.refresh();
    ready(w);
    check(copy(w) == "clipboard sentinel", "revision invalidates a word selection");
    const auto signaturePoint = w.canvas->pdfToViewport(0, s.rect.center()).toPoint();
    hover(w, signaturePoint);
    check(w.canvas->viewport()->cursor().shape() == Qt::SizeAllCursor,
          "signature hover has priority");
    wordClick(w, 0, s.rect.center());
    check(w.canvas->selected >= 0 && w.canvas->copied.isEmpty(),
          "signature double click does not select underlying words");
    w.canvas->setHandTool(true);
    hover(w, position);
    check(w.canvas->viewport()->cursor().shape() == Qt::OpenHandCursor, "hand tool has priority");
    wordClick(w, 0, point);
    check(w.canvas->copied.isEmpty(), "hand double click does not select text");
    w.canvas->setHandTool(false);
    w.undoAction->trigger();
    w.canvas->beginPlacement();
    hover(w, position);
    check(w.canvas->viewport()->cursor().shape() == Qt::CrossCursor, "placement has priority");
    w.canvas->cancelInteraction();
    w.doc.open(fixtures + "/viewer-selection-restricted.pdf", "selection-user");
    w.refresh(true);
    ready(w);
    wordClick(w, 0, glyph(w, 0, "ALPHA"));
    check(copy(w) == "clipboard sentinel" && w.canvas->copied.isEmpty(),
          "word selection enforces copy permission");
    check(w.canvas->viewport()->cursor().shape() != Qt::IBeamCursor,
          "forbidden copy has no I-beam");
    w.openFile(fixtures + "/D03.pdf");
    w.canvas->fitPage();
    ready(w);
    wordClick(w, 0, w.doc.pdf().getCatalog()->getPage(0)->getCropBox().center());
    check(w.canvas->copied.isEmpty() && w.canvas->selectionMessage().contains("OCR"),
          "scanned page explains OCR instead of pretending to select");
    // Issue and cancel before dispatching worker completion on the GUI event loop.
    w.openFile(fixtures + "/viewer-search.pdf");
    const auto pending = w.canvas->pdfToViewport(0, glyph(w, 0, "opens")).toPoint();
    QMouseEvent press(QEvent::MouseButtonDblClick, pending, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(w.canvas->viewport(), &press);
    check(!w.canvas->selectionReady(), "first word click waits asynchronously");
    QTest::keyClick(w.canvas->viewport(), Qt::Key_Escape);
    check(QTest::qWaitFor([&] { return w.canvas->selectionExtractedPages() > 0; }, 10000),
          "cancelled extraction eventually completes");
    check(w.canvas->copied.isEmpty() && copy(w) == "clipboard sentinel",
          "late extraction never restores a cancelled word selection");
    w.signatureAction->trigger();
    QCoreApplication::processEvents();
    w.signature->setPlainText("opens this");
    QTextCursor cursor(w.signature->document());
    cursor.setPosition(2);
    const auto textPoint = w.signature->cursorRect(cursor).center();
    QTest::mouseDClick(w.signature->viewport(), Qt::LeftButton, Qt::NoModifier, textPoint);
    QTest::mouseRelease(w.signature->viewport(), Qt::LeftButton, Qt::NoModifier, textPoint);
    QApplication::clipboard()->setText("input sentinel");
    QTest::keyClick(w.signature, Qt::Key_C, Qt::ControlModifier);
    check(QApplication::clipboard()->text() == "opens" && w.canvas->copied.isEmpty(),
          "input field double click and copy remain local to the input");
    w.openFile(fixtures + "/viewer-navigation.pdf");
    w.canvas->fitPage();
    ready(w);
    const auto link = w.canvas->pdfToViewport(0, {180, 585}).toPoint();
    hover(w, link);
    check(w.canvas->viewport()->cursor().shape() == Qt::PointingHandCursor,
          "link cursor overrides underlying text");
    QTest::mouseClick(w.canvas->viewport(), Qt::LeftButton, Qt::ShiftModifier, link);
    ready(w, 2);
    const auto destination = w.canvas->page;
    QTest::mouseDClick(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, link);
    QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, link);
    check(w.canvas->page == destination && w.canvas->copied.isEmpty(),
          "a second link click does not select unrelated destination text");
    return {{"rotation_crop_userunit", true},
            {"pointer_priorities", true},
            {"blank_and_scan", true},
            {"revision_and_pending_cancel", true},
            {"copy_permission", true},
            {"input_field_and_link", true},
            {"OS_input", "未実行; Qt synthetic events"}};
}
} // namespace tatsu
