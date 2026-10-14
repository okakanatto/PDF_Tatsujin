#include "navigation_tests.h"
#include "pdfoutline.h"
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
          QString("PDF page %1 ready, current=%2").arg(page + 1).arg(window.canvas->page + 1));
}
QTreeWidget* tree(Window& window)
{
    auto widget = window.findChild<QTreeWidget*>("bookmarkTree");
    window.findChild<QTabWidget*>("navigationTabs")->setCurrentWidget(widget->parentWidget());
    return widget;
}
QTreeWidgetItem* item(Window& window, const QString& title)
{
    auto widget = tree(window);
    for (QTreeWidgetItemIterator it(widget); *it; ++it)
        if ((*it)->text(0) == title)
            return *it;
    fail("bookmark missing: " + title);
}
void activate(Window& window, const QString& title, bool keyboard = false)
{
    auto widget = tree(window);
    auto target = item(window, title);
    QCoreApplication::processEvents();
    widget->scrollToItem(target);
    widget->setCurrentItem(target);
    QCoreApplication::processEvents();
    if (keyboard)
    {
        window.activateWindow();
        widget->setFocus();
        QTest::keyClick(widget, Qt::Key_Return);
    }
    else
    {
        const auto point =
            widget->visualItemRect(target).intersected(widget->viewport()->rect()).center();
        check(widget->viewport()->rect().contains(point) && widget->itemAt(point) == target,
              "bookmark row is visible and hit-testable: " + title);
        QTest::mouseClick(widget->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    }
}
double error(Canvas* canvas, const ViewState& state)
{
    const auto actual = canvas->pdfToViewport(state.anchor.page, state.anchor.point);
    const QPointF expected(canvas->viewport()->width() * state.anchor.ratio.x(),
                           canvas->viewport()->height() * state.anchor.ratio.y());
    return qMax(qAbs(actual.x() - expected.x()), qAbs(actual.y() - expected.y()));
}
QAction* back(Window& window)
{
    return window.findChild<QAction*>("previousView");
}
void notice(Window& window, const std::function<void()>& operation, const QString& expected,
            const QString& screenshot = {})
{
    QString text;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout,
                     [&]
                     {
                         for (auto widget : QApplication::topLevelWidgets())
                             if (auto message = qobject_cast<QMessageBox*>(widget);
                                 message && message->isVisible())
                             {
                                 check(message->textFormat() == Qt::PlainText,
                                       "PDF-provided notice uses plain text");
                                 text = message->text();
                                 if (!screenshot.isEmpty())
                                     message->grab().save(screenshot);
                                 message->accept();
                             }
                     });
    timer.start(20);
    const auto state = window.canvas->viewState();
    const bool history = back(window)->isEnabled();
    operation();
    timer.stop();
    check(text.contains(expected),
          "unsupported notice, expected=" + expected + ", actual=" + text +
              ", current page=" + QString::number(window.canvas->page + 1));
    check(error(window.canvas, state) <= 2 && qAbs(window.canvas->zoom - state.zoom) < .001 &&
              back(window)->isEnabled() == history,
          "unsupported target does not navigate or add history");
}
void centerLink(Window& window)
{
    window.canvas->goToPage(0);
    window.canvas->setZoom(1.2);
    window.canvas->restoreAnchor({0, {190, 590}, {.5, .5}});
    ready(window);
}
void clickPdf(Window& window, QPointF point)
{
    const auto device = window.canvas->pdfToViewport(0, point).toPoint();
    check(window.canvas->viewport()->rect().contains(device), "link visible before click");
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, device);
}
} // namespace

QJsonObject testNavigationBookmarks(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(1024, 720);
    window.show();
    window.openFile(fixtures + "/viewer-navigation.pdf");
    ready(window);
    auto widget = tree(window);
    check(widget->topLevelItemCount() == 13 && widget->topLevelItem(0)->childCount() == 4,
          "frozen hierarchical outline count");
    const QStringList labels{"i", "ii", "本編-7", "本編-8", "AA", "BB"};
    check(readPageLabels(window.doc.pdf()) == labels, "PDF page label styles and start values");
    for (int page = 0; page < labels.size(); ++page)
        check(window.pages->item(page)->text() == pageDescription(page, labels),
              "page label also displays physical page number");
    const auto original = encodePdf(window.doc.pdf());
    QJsonArray coordinates;
    for (int page = 0; page < 4; ++page)
    {
        const auto before = window.canvas->viewState();
        activate(window, QString("回転%1°：中心へ").arg(page * 90), page % 2);
        ready(window, page);
        check(window.canvas->page == page && qAbs(window.canvas->zoom - 3) < .001,
              "bookmark reaches physical page and XYZ zoom");
        auto point = window.canvas->pdfToViewport(page, {300, 400});
        const auto displacement = qMax(qAbs(point.x()), qAbs(point.y()));
        check(displacement <= 2,
              QString("XYZ rotated point at top-left: %1 DIP").arg(displacement));
        back(window)->trigger();
        ready(window, before.anchor.page);
        const auto restoreError = error(window.canvas, before);
        check(restoreError <= 2 && window.canvas->fitMode() == before.fitMode &&
                  qAbs(window.canvas->zoom - before.zoom) < .001,
              "Back restores reading point and mode");
        window.findChild<QAction*>("nextView")->trigger();
        ready(window, page);
        check(qAbs(window.canvas->zoom - 3) < .001, "Forward restores destination zoom");
        coordinates.append(QJsonObject{
            {"page", page + 1}, {"XYZ_error_DIP", displacement}, {"back_error_DIP", restoreError}});
    }
    activate(window, "UserUnit 2");
    ready(window, 5);
    auto point = window.canvas->pdfToViewport(5, {300, 400});
    check(qMax(qAbs(point.x()), qAbs(point.y())) <= 2, "XYZ includes UserUnit 2");
    const QString namedTitle =
        "名前付き宛先：日本語の長いしおりタイトルを省略せずツールチップで確認する";
    check(item(window, namedTitle)->toolTip(0).contains(namedTitle) &&
              item(window, namedTitle)->toolTip(0).contains("本編-7"),
          "full title and destination tooltip");
    activate(window, namedTitle);
    ready(window, 2);
    check(window.canvas->page == 2, "name tree resolves to page 3");
    for (const auto& title : QStringList{"全体 Fit", "幅 FitH", "高さ FitV", "矩形 FitR"})
    {
        activate(window, title);
        ready(window, 4);
        check(window.canvas->page == 4, "fit destination page");
        if (title == "全体 Fit")
            check(window.canvas->fitMode() == 2, "Fit uses whole-page mode");
        if (title == "幅 FitH")
            check(window.canvas->fitMode() == 1, "FitH uses width mode");
        if (title == "矩形 FitR")
        {
            const auto p1 = window.canvas->pdfToViewport(4, {180, 250});
            const auto p2 = window.canvas->pdfToViewport(4, {420, 550});
            const QRectF rectangle(p1, p2);
            check(QRectF(window.canvas->viewport()->rect()).contains(rectangle.normalized()),
                  "frozen FitR rectangle is fully visible");
        }
    }
    window.canvas->setZoom(2);
    window.canvas->restoreAnchor({4, {250, 500}, {0, 0}});
    const auto nullBefore = window.canvas->anchor({0, 0});
    activate(window, "null XYZ：位置と倍率を保持");
    check(qAbs(window.canvas->zoom - 2) < .001 &&
              QLineF(window.canvas->anchor({0, 0}).point, nullBefore.point).length() < 1,
          "null XYZ retains current point and zoom");
    for (const auto& title : QStringList{"存在しない名前", "循環した名前", "無効なページ参照",
                                         "BBox 未対応", "複合アクション 未対応"})
        notice(
            window,
            [&]
            {
                window.grab().save(output + "/before-invalid-bookmark.png");
                activate(window, title);
            },
            title.contains("名前") ? (title.contains("循環") ? "循環" : "見つけられません")
            : title.contains("参照") ? "文書内にありません"
                                     : "未対応");
    activate(window, namedTitle);
    const auto destinationState = window.canvas->viewState();
    activate(window, namedTitle);
    back(window)->trigger();
    check(window.canvas->page != destinationState.anchor.page,
          "repeat activation adds no redundant history");
    window.signatureAction->trigger();
    QCoreApplication::processEvents();
    tree(window);
    window.canvas->fitPage();
    window.canvas->goToPage(0);
    const auto tabs = window.findChild<QTabWidget*>("navigationTabs")->tabBar();
    check(!tabs->usesScrollButtons() || tabs->tabRect(2).right() < tabs->width(),
          "all three navigation tabs fit minimum window");
    window.grab().save(output + "/bookmarks-1024.png");
    check(encodePdf(window.doc.pdf()) == original && window.doc.cursor == 0 && !window.doc.dirty(),
          "navigation never alters PDF or edit history");
    return {{"coordinates", coordinates},
            {"hierarchy", "13 roots; 4 children"},
            {"page_labels", QJsonArray::fromStringList(labels)},
            {"FitR", true},
            {"null_XYZ", true},
            {"invalid_actions", 5},
            {"PDF_and_undo_unchanged", true}};
}

QJsonObject testNavigationLinks(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/viewer-navigation.pdf");
    ready(window);
    centerLink(window);
    auto canvas = window.canvas;
    auto viewport = canvas->viewport();
    const auto original = encodePdf(window.doc.pdf());
    const auto before = canvas->viewState();
    clickPdf(window, {180, 585});
    ready(window, 2);
    check(canvas->page == 2 && qAbs(canvas->zoom - 3) < .001, "actual annotation named link click");
    back(window)->trigger();
    check(error(canvas, before) <= 2, "link Back preserves reading point");
    centerLink(window);
    const auto start = canvas->pdfToViewport(0, {90, 586}).toPoint();
    const auto end = canvas->pdfToViewport(0, {290, 574}).toPoint();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(viewport, end, 10);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
    check(QTest::qWaitFor([&] { return canvas->selectionReady(); }, 10000),
          "link range text ready");
    check(canvas->page == 0 && canvas->copied.contains("LINK:"),
          "link drag selects text without navigation");
    for (auto type : {QEvent::FocusOut, QEvent::WindowDeactivate})
    {
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
        QEvent event(type);
        QApplication::sendEvent(
            type == QEvent::FocusOut ? static_cast<QObject*>(viewport) : &window, &event);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start);
        check(canvas->page == 0, "focus/deactivation cancels pending link");
    }
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QTest::keyClick(viewport, Qt::Key_Escape);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start);
    check(canvas->page == 0, "Escape cancels pending link");
    window.findChild<QAction*>("handReadingTool")->trigger();
    clickPdf(window, {180, 585});
    check(canvas->page == 0, "hand click does not activate link");
    window.findChild<QAction*>("selectReadingTool")->trigger();
    notice(
        window, [&] { clickPdf(window, {180, 545}); }, "https://example.invalid/document?x=<text>",
        output + "/external-link-notice.png");
    notice(window, [&] { clickPdf(window, {180, 505}); }, "スクリプトは実行しません");
    clickPdf(window, {180, 465});
    check(canvas->page == 0, "hidden annotation never activates");
    clickPdf(window, {300, 623});
    check(canvas->page == 0, "outside QuadPoints never activates despite Rect");
    clickPdf(window, {150, 623});
    ready(window, 1);
    check(canvas->page == 1, "inside QuadPoints activates link");
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty() && window.doc.cursor == 0,
          "all link operations leave PDF and undo unchanged");
    window.doc.open(fixtures + "/viewer-navigation-restricted.pdf", "navigation-user");
    window.refresh(true);
    centerLink(window);
    const auto protectedPdf = window.doc.pdf();
    const auto protectedHash = fileHash(window.doc.source);
    check(!window.doc.copyAllowed && !window.doc.readOnly.isEmpty(),
          "copy-restricted read-only fixture");
    const auto restrictedStart = canvas->pdfToViewport(0, {180, 585}).toPoint();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, restrictedStart);
    QEvent out(QEvent::FocusOut);
    QApplication::sendEvent(viewport, &out);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, restrictedStart);
    check(canvas->page == 0, "copy-restricted pending link also cancels on focus loss");
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, restrictedStart);
    canvas->setZoom(1.3);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, restrictedStart);
    check(canvas->page == 0, "copy-restricted pending link cancels when zoom changes");
    centerLink(window);
    clickPdf(window, {180, 585});
    ready(window, 2);
    check(canvas->page == 2 && window.doc.pdf() == protectedPdf &&
              fileHash(window.doc.source) == protectedHash && !window.doc.dirty(),
          "restricted link navigates without decryption or modification");
    return {{"named_annotation_click", true},
            {"drag_selects_text", true},
            {"Esc_focus_deactivate", "synthetic Qt events"},
            {"QuadPoints_and_hidden", true},
            {"external_script_notice", true},
            {"protected_PDF_unchanged", true}};
}

QJsonObject testNavigationLifecycle(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/viewer-navigation.pdf");
    ready(window);
    auto widget = tree(window);
    widget->topLevelItem(0)->setExpanded(false);
    auto added = window.doc.putSignature(0, "山田 太郎", {100, 580}, 14, Qt::black);
    window.refresh();
    check(!widget->topLevelItem(0)->isExpanded(), "edit retains collapsed bookmark group");
    centerLink(window);
    const auto start = window.canvas->pdfToViewport(0, added.rect.center()).toPoint();
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window.canvas->viewport(), start + QPoint(30, 30), 10);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                        start + QPoint(30, 30));
    check(window.doc.cursor == 2 && window.canvas->page == 0,
          "signature on link has editing priority");
    window.undoAction->trigger();
    window.doc.rotate(0);
    window.refresh();
    activate(window, "UserUnit 2");
    ready(window, 5);
    window.undoAction->trigger();
    check(signatures(window.doc.pdf(), 0).size() == 1 && window.doc.cursor == 1,
          "navigation adds no edit and Undo removes rotation only");
    centerLink(window);
    window.doc.busy = true;
    window.refresh();
    clickPdf(window, {260, 585});
    ready(window, 2);
    check(window.canvas->page == 2, "busy permits link reading");
    window.doc.busy = false;
    window.refresh();
    window.doc.save(output + "/navigation-preserved.pdf");
    window.openFile(output + "/navigation-preserved.pdf");
    ready(window);
    check(signatures(window.doc.pdf(), 0).size() == 1 && tree(window)->topLevelItemCount() == 13,
          "signature and outline survive save and reopen");
    check(readPageLabels(window.doc.pdf()) ==
              QStringList{"i", "ii", "本編-7", "本編-8", "AA", "BB"},
          "labels survive save and reopen");
    window.canvas->fitPage();
    window.canvas->goToPage(0);
    tree(window);
    ready(window);
    window.grab().save(output + "/navigation-signature.png");
    centerLink(window);
    clickPdf(window, {260, 585});
    ready(window, 2);
    check(window.canvas->page == 2, "saved link still operates");
    auto savedSignature = signatures(window.doc.pdf(), 0).first();
    window.doc.moveSignature(0, savedSignature, {0, -20});
    window.refresh();
    window.undoAction->trigger();
    check(signatures(window.doc.pdf(), 0).first().rect == savedSignature.rect,
          "saved signature remains editable with Undo");
    window.openFile(fixtures + "/D01.pdf");
    ready(window);
    check(tree(window)->topLevelItemCount() == 0, "opening another PDF discards old outline");
    check(!back(window)->isEnabled() && !window.findChild<QAction*>("nextView")->isEnabled(),
          "opening another PDF clears both reading history directions");
    window.openFile(fixtures + "/D07.pdf");
    ready(window);
    check(tree(window)->topLevelItemCount() > 0, "original acceptance document has usable outline");
    auto first = tree(window)->topLevelItem(0)->text(0);
    activate(window, first);
    check(window.canvas->page == 0, "D07 outline resolves its existing page");
    return {{"signature_priority_and_Undo", true},
            {"rotate_revision", true},
            {"save_reopen_reedit", true},
            {"outline_label_link_preserved", true},
            {"document_replacement", true},
            {"busy", "state simulation; actual OCR is in A05-A09 regression"}};
}
QJsonObject testViewHistory()
{
    using Direction = ViewHistory::Direction;
    ViewHistory history;
    auto entry = [](int page)
    { return ViewHistory::Entry{{{page, {100, 200}, {.5, .5}}, 1.25, 0, 0, 17}, "alpha", 3}; };
    check(!history.move(Direction::Back, entry(0)), "empty history is inert");
    for (int page = 0; page < 205; ++page)
        history.remember(entry(page));
    for (int page = 204; page >= 5; --page)
    {
        const auto restored = history.move(Direction::Back, entry(page + 1));
        check(restored && restored->view.anchor.page == page,
              "bounded Back keeps the latest 200 reading positions in order");
    }
    check(!history.canMove(Direction::Back) && history.canMove(Direction::Forward),
          "oldest five positions were evicted");
    for (int page = 6; page <= 205; ++page)
    {
        const auto restored = history.move(Direction::Forward, entry(page - 1));
        check(restored && restored->view.anchor.page == page,
              "Forward restores the entire bounded path in order");
    }
    check(!history.canMove(Direction::Forward), "Forward stops at the current view");
    history.clear();
    const auto original = entry(0);
    auto nearby = original;
    nearby.view.anchor.point += QPointF(.2, .2);
    nearby.view.zoom += .0005;
    history.remember(original);
    history.remember(nearby);
    check(history.move(Direction::Back, entry(1))->view.anchor.point ==
                  original.view.anchor.point &&
              !history.canMove(Direction::Back),
          "layout noise does not add a duplicate or replace the original reading position");
    history.remember(entry(2));
    check(!history.canMove(Direction::Forward), "new navigation discards the abandoned branch");
    ViewHistory::Entry invalid;
    history.remember(invalid);
    check(!history.move(Direction::Back, invalid) && history.canMove(Direction::Back),
          "unloaded reading positions do not consume valid history");
    history.clear();
    QVector<ViewHistory::Entry> distinct;
    distinct.append(original);
    auto changed = original;
    changed.query = "beta";
    distinct.append(changed);
    changed.revision = 4;
    distinct.append(changed);
    changed.view.activeSearch = 18;
    distinct.append(changed);
    changed.view.fitMode = 1;
    distinct.append(changed);
    changed.view.fitReference = 2;
    distinct.append(changed);
    changed.view.anchor.ratio = {0, 0};
    distinct.append(changed);
    for (const auto& position : distinct)
        history.remember(position);
    for (auto position = distinct.crbegin(); position != distinct.crend(); ++position)
    {
        const auto restored = history.move(Direction::Back, entry(5));
        check(restored && restored->query == position->query &&
                  restored->revision == position->revision &&
                  restored->view.activeSearch == position->view.activeSearch &&
                  restored->view.fitMode == position->view.fitMode &&
                  restored->view.fitReference == position->view.fitReference &&
                  restored->view.anchor.ratio == position->view.anchor.ratio,
              "query, revision, occurrence, fit reference and anchor remain distinct");
    }
    check(!history.canMove(Direction::Back), "all distinct entries restored exactly once");
    check(original.activeSearchFor("alpha", 3) == 17 && original.activeSearchFor("beta", 3) == 0 &&
              original.activeSearchFor("alpha", 4) == 0,
          "a search occurrence is restored only for its original query and document revision");
    history.clear();
    check(!history.canMove(Direction::Back) && !history.canMove(Direction::Forward),
          "document replacement clears both directions");
    return {{"bounded_positions", 200},         {"back_forward_order", true},
            {"duplicate_noise", true},          {"branch_invalidation", true},
            {"fit_reference_and_anchor", true}, {"stale_search", true}};
}
} // namespace tatsu
