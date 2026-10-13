#include "search_tests.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& error)
{
    if (!value)
        fail(error);
}
SearchPanel* panel(Window& window)
{
    return window.findChild<SearchPanel*>("searchPanel");
}
void complete(Window& window)
{
    check(QTest::qWaitFor(
              [&]
              {
                  return panel(window)->session()->complete() &&
                         panel(window)->session()->totalPages() == window.doc.pages();
              },
              15000),
          "search finishes for the current document");
    check(panel(window)->session()->errors().isEmpty(), "all pages parsed without search errors");
}
void ready(Window& window, int page = 0)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 10000), "PDF is visible");
}
void search(Window& window, const QString& term)
{
    window.query->setText(term);
    QTest::keyClick(window.query, Qt::Key_Return);
    complete(window);
}
double positionError(Canvas* canvas, const ViewState& state)
{
    const auto actual = canvas->pdfToViewport(state.anchor.page, state.anchor.point);
    const QPointF expected(canvas->viewport()->width() * state.anchor.ratio.x(),
                           canvas->viewport()->height() * state.anchor.ratio.y());
    return qMax(qAbs(actual.x() - expected.x()), qAbs(actual.y() - expected.y()));
}
} // namespace

QJsonObject testSearchNavigation(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/viewer-search.pdf");
    ready(window);
    auto p = panel(window);
    const auto original = encodePdf(window.doc.pdf());
    const auto before = window.canvas->viewState();
    search(window, "alpha");
    check(p->session()->rowCount() == 4, "four frozen case-insensitive English occurrences");
    const QVector<int> expectedPages{0, 0, 1, 2};
    for (int i = 0; i < 4; ++i)
        check(p->session()->matches()[i].page == expectedPages[i],
              "matches are in physical page order");
    check(p->session()->indexOf(p->activeMatch()) == 0, "Enter opens the first occurrence");
    auto first = window.canvas->viewState();
    QTest::keyClick(window.query, Qt::Key_Return);
    check(p->session()->indexOf(p->activeMatch()) == 1,
          "Enter reaches the second occurrence on the same page");
    auto second = window.canvas->viewState();
    check(QLineF(first.anchor.point, second.anchor.point).length() > 50,
          "the second occurrence scrolls to its own context");
    auto back = window.findChild<QAction*>("previousView");
    auto forward = window.findChild<QAction*>("nextView");
    check(back->isEnabled(), "view history can return from search");
    back->trigger();
    QCoreApplication::processEvents();
    const double firstError = positionError(window.canvas, first);
    check(firstError <= 2, QString("Back restores first match point: %1 DIP").arg(firstError));
    check(p->session()->indexOf(p->activeMatch()) == 0,
          "Back restores the active occurrence in both list and PDF");
    forward->trigger();
    QCoreApplication::processEvents();
    const double secondError = positionError(window.canvas, second);
    check(secondError <= 2,
          QString("Forward restores second match point: %1 DIP").arg(secondError));
    check(p->session()->indexOf(p->activeMatch()) == 1,
          "Forward restores the second active occurrence");
    back->trigger();
    back->trigger();
    QCoreApplication::processEvents();
    const double originError = positionError(window.canvas, before);
    check(originError <= 2,
          QString("Back restores pre-search reading point: %1 DIP").arg(originError));
    check(window.canvas->fitMode() == before.fitMode &&
              qAbs(window.canvas->zoom - before.zoom) < .005,
          "history restores the fit mode and zoom");
    window.pages->setCurrentRow(1);
    check(!forward->isEnabled(), "new explicit navigation clears forward history");
    search(window, "交通費");
    check(p->session()->rowCount() == 4, "four frozen Japanese occurrences");
    QTest::keyClick(window.query, Qt::Key_Return, Qt::ShiftModifier);
    // The first result may be on the prioritised current page; the list remains sorted.
    check(p->session()->indexOf(p->activeMatch()) >= 0, "Shift+Enter navigates backward");
    search(window, "expense");
    check(p->session()->rowCount() == 1 && p->session()->matches()[0].page == 1,
          "changing a query and immediately pressing Enter uses the new query");
    window.canvas->goToPage(0);
    QTest::keyClick(window.query, Qt::Key_Return);
    check(window.canvas->page == 1,
          "next match returns to the single occurrence after manually leaving its page");
    window.canvas->setZoom(.65);
    search(window, "alpha");
    auto results = p->findChild<QListView*>("searchResults");
    results->setCurrentIndex(p->session()->index(3));
    ready(window, 2);
    const auto point = window.canvas->pdfToViewport(2, QPointF(60, 330));
    check(point.x() >= -2 && point.x() < window.canvas->viewport()->width(),
          "search on a wider page keeps the line beginning in view when the page fits");
    check(qAbs(window.canvas->zoom - .65) < .0001, "search navigation preserves explicit zoom");
    window.grab().save(output + "/search-results.png");
    window.resize(1024, 720);
    window.signatureAction->trigger();
    QTest::qWait(100);
    for (auto widget : {static_cast<QWidget*>(window.query),
                        static_cast<QWidget*>(p->findChild<QPushButton*>("previousMatch")),
                        static_cast<QWidget*>(p->findChild<QPushButton*>("nextMatch")),
                        static_cast<QWidget*>(results)})
        check(widget->isVisible() &&
                  window.rect().contains(QRect(widget->mapTo(&window, QPoint()), widget->size())),
              "search controls remain visible at 1024x720 with signature settings open");
    window.grab().save(output + "/search-1024.png");
    QTest::keyClick(window.query, Qt::Key_Return);
    check(p->session()->indexOf(p->activeMatch()) == 0, "complete search wraps at the end");
    check(p->findChild<QLabel*>("searchSummary")->text().contains("先頭へ"), "wrap is announced");
    search(window, "no-such-text-92817");
    check(p->session()->rowCount() == 0, "zero hits clear old results");
    window.query->clear();
    check(p->session()->rowCount() == 0 && p->activeMatch() == 0, "clear removes active match");
    check(!window.doc.dirty() && window.doc.cursor == 0 && encodePdf(window.doc.pdf()) == original,
          "search and view history never change the PDF or edit Undo");
    return {{"English_occurrences", 4},
            {"Japanese_occurrences", 4},
            {"history_error_DIP", QJsonArray{firstError, secondError, originError}},
            {"PDF_and_undo_unchanged", true}};
}

QJsonObject testSearchGeneration(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D10-digital-100.pdf");
    ready(window);
    window.canvas->goToPage(49);
    ready(window, 49);
    auto p = panel(window);
    quint64 firstActive = 0;
    int ticks = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start(5);
    QObject::connect(p->session(), &SearchSession::updated, &window,
                     [&]
                     {
                         if (!firstActive && p->activeMatch())
                             firstActive = p->activeMatch();
                     });
    QElapsedTimer searchTimer;
    searchTimer.start();
    search(window, "English");
    const auto firstSearchMs = searchTimer.elapsed();
    heartbeat.stop();
    check(p->session()->rowCount() == 100, "one frozen occurrence on each of 100 pages");
    check(firstActive && p->activeMatch() == firstActive,
          "inserting earlier pages does not change the selected occurrence");
    check(p->session()->matches()[p->session()->indexOf(firstActive)].page == 49,
          "current page is searched first");
    check(ticks > 0, "UI timer events continue during background search");
    window.query->setText("English");
    QTest::keyClick(window.query, Qt::Key_Return);
    window.query->setText("no-such-text-92817");
    QTest::keyClick(window.query, Qt::Key_Return);
    window.query->setText("日本語");
    QTest::keyClick(window.query, Qt::Key_Return);
    complete(window);
    check(p->session()->rowCount() == 100, "only the latest queued query is published");
    for (const auto& match : p->session()->matches())
        check(match.excerpt.contains("日本語"), "latest query excerpts remain coherent");
    window.openFile(fixtures + "/viewer-search.pdf");
    ready(window);
    search(window, "alpha");
    check(p->session()->rowCount() == 4, "opening another PDF discards old search generation");
    window.doc.putSignature(0, "山田 太郎", {100, 200}, 14, Qt::black);
    window.refresh();
    complete(window);
    check(p->session()->rowCount() == 4 && p->activeMatch() == 0,
          "revision changes refresh results without an unsolicited jump");
    window.undoAction->trigger();
    complete(window);
    check(p->session()->rowCount() == 4, "Undo restarts search against the restored snapshot");
    window.openFile(fixtures + "/D02.pdf");
    ready(window);
    search(window, "PDF");
    check(p->session()->rowCount() == 3,
          "CropBox excludes the fourth page's fully cropped-away text");
    auto list = p->findChild<QListView*>("searchResults");
    for (int i = 0; i < 3; ++i)
    {
        list->setCurrentIndex(p->session()->index(i));
        ready(window, i);
        auto match = p->session()->matches()[i];
        auto center = window.canvas->pdfToViewport(i, match.bounds.center());
        check(window.canvas->viewport()->rect().contains(center.toPoint()),
              "rotated search result is actually visible");
    }
    window.openFile(fixtures + "/D03.pdf");
    ready(window);
    search(window, "図書館");
    check(p->session()->rowCount() == 0 && p->session()->textlessPages() == 8,
          "eight image-only pages are identified for the OCR hint");
    auto closing = std::make_unique<Window>();
    closing->openFile(fixtures + "/D10-digital-100.pdf");
    closing->query->setText("English");
    QTest::keyClick(closing->query, Qt::Key_Return);
    QElapsedTimer timer;
    timer.start();
    closing.reset();
    const auto closeMs = timer.elapsed();
    QCoreApplication::processEvents();
    window.grab().save(output + "/search-generation.png");
    return {{"current_page_first", 50},          {"search_100_pages_ms", firstSearchMs},
            {"stable_active_match", true},       {"UI_timer_ticks", ticks},
            {"active_search_close_ms", closeMs}, {"OS_input", "未実行; Qt synthetic events"}};
}

QJsonObject testSearchInput(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/viewer-search.pdf");
    ready(window);
    auto p = panel(window);
    auto tabs = window.findChild<QTabWidget*>("navigationTabs");
    tabs->setCurrentWidget(p);
    window.query->setFocus();
    QInputMethodEvent preedit("交通費", {});
    QApplication::sendEvent(window.query, &preedit);
    QTest::keyClick(window.query, Qt::Key_Return);
    QTest::qWait(200);
    check(p->session()->totalPages() == 0, "preedit and Return do not start a search");
    QInputMethodEvent commit;
    commit.setCommitString("交通費");
    QApplication::sendEvent(window.query, &commit);
    QTest::keyClick(window.query, Qt::Key_Return);
    complete(window);
    check(window.query->text() == "交通費" && p->session()->rowCount() == 4,
          "committed Japanese query is searched");
    const auto active = p->activeMatch();
    QTest::keyClick(window.query, Qt::Key_Escape);
    check(window.canvas->hasFocus() || window.canvas->viewport()->hasFocus(),
          "Escape returns focus to the document");
    check(p->activeMatch() == active && p->session()->rowCount() == 4,
          "Escape preserves query and results");
    window.grab().save(output + "/search-japanese.png");
    return {{"composition_events", "synthetic Qt QInputMethodEvent"},
            {"native_IME", "未実行"},
            {"Escape_keeps_results", true}};
}
} // namespace tatsu
