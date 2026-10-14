#include "reference_workflow_tests.h"
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
void ready(Window& window, int page)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 10000),
          "reference workflow renders the actual PDF");
}
void settle()
{
    QTest::qWait(70);
}
SearchPanel* search(Window& window, const QString& term)
{
    // QShortcut dispatch requires an active Qt window even for synthetic keys.
    // Resizing an offscreen top-level can change its activation asynchronously.
    window.activateWindow();
    check(QTest::qWaitFor([&] { return window.isActiveWindow(); }, 1000),
          "reference keyboard input targets the active test window");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    auto panel = window.findChild<SearchPanel*>("searchPanel");
    auto tabs = window.findChild<QTabWidget*>("navigationTabs");
    check(QTest::qWaitFor([&] { return tabs->currentWidget() == panel; }, 2000),
          "actual Find shortcut opens the search panel");
    window.query->setText(term);
    QTest::keyClick(window.query, Qt::Key_Return);
    check(QTest::qWaitFor(
              [&] {
                  return panel->session()->complete() &&
                         panel->session()->totalPages() == window.doc.pages();
              },
              15000),
          "search completes for the current PDF");
    check(panel->session()->errors().isEmpty(), "reference search has no page errors");
    settle();
    return panel;
}
double positionError(Window& window, const ViewState& state)
{
    const auto point = window.canvas->pdfToViewport(state.anchor.page, state.anchor.point);
    return qMax(qAbs(point.x() - window.canvas->viewport()->width() * state.anchor.ratio.x()),
                qAbs(point.y() - window.canvas->viewport()->height() * state.anchor.ratio.y()));
}
void clickResult(QListView* list, int row)
{
    const auto index = list->model()->index(row, 0);
    list->scrollTo(index);
    settle();
    const auto rect = list->visualRect(index).intersected(list->viewport()->rect());
    check(!rect.isEmpty() && list->indexAt(rect.center()) == index,
          "result row is actually clickable");
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
}
void writeRows(const QString& path, const QJsonArray& rows)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "write reference diagnostics");
    file.write(QJsonDocument(rows).toJson());
}
void openReferenceMenu(Window& window, const QString& actionName)
{
    auto control = window.findChild<QToolButton*>("referenceControl");
    auto action = window.findChild<QAction*>(actionName);
    check(control && action && control->isVisible() && control->isEnabled(),
          "fixed reference entry is available");
    bool selected = false;
    QTimer choose;
    choose.setInterval(10);
    QObject::connect(&choose, &QTimer::timeout,
                     [&]
                     {
                         if (control->menu()->isVisible())
                         {
                             control->menu()->setActiveAction(action);
                             QTest::keyClick(control->menu(), Qt::Key_Return);
                             selected = true;
                             choose.stop();
                         }
                     });
    choose.start();
    control->showMenu();
    check(QTest::qWaitFor([&] { return selected; }, 2000),
          "reference popup accepts a keyboard choice");
    settle();
}
} // namespace
QJsonObject testCompactReferences(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.activateWindow();
    window.openFile(fixtures + "/viewer-navigation.pdf");
    ready(window, 0);
    window.doc.putSignature(0, "山田 太郎", {100, 200}, 14, Qt::black);
    window.refresh();
    const auto document = encodePdf(window.doc.pdf());
    const auto hash = fileHash(window.doc.source);
    const auto cursor = window.doc.cursor;
    auto tabs = window.findChild<QTabWidget*>("navigationTabs");
    auto dock = window.findChild<QDockWidget*>("navigationDock");
    auto tree = window.findChild<QTreeWidget*>("bookmarkTree");
    QJsonArray cases;
    for (const auto size : {QSize(800, 480), QSize(960, 540), QSize(1024, 720), QSize(1280, 850)})
    {
        window.resize(size);
        window.signatureAction->trigger();
        window.signature->setPlainText("山田 太郎\n入力途中は残す");
        search(window, "LINK");
        check(dock->isVisible(), "search navigation is visible with settings");
        auto bar = tabs->tabBar();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(1).center());
        settle();
        cases.append(QJsonObject{{"width", size.width()},
                                 {"height", size.height()},
                                 {"bookmark_tab_visible", dock->isVisible()},
                                 {"bookmark_tree_visible", tree->isVisible()}});
        writeRows(output + "/compact-reference-steps.json", cases);
        window.grab().save(output + "/compact-reference-latest.png");
        check(dock->isVisible() && tree->isVisible() && !tree->visibleRegion().isEmpty(),
              "choosing bookmarks must not hide the navigation panel");
        tree->setFocus();
        QTreeWidgetItem* destination = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == "UserUnit 2")
                destination = *it;
        check(destination, "frozen UserUnit bookmark is present");
        for (auto parent = destination->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
        tree->setCurrentItem(destination);
        tree->scrollToItem(destination);
        QTest::keyClick(tree, Qt::Key_Return);
        ready(window, 5);
        settle();
        check(window.signature->toPlainText() == "山田 太郎\n入力途中は残す",
              "reference navigation retains the signature draft");
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
        settle();
        check(dock->isVisible() && window.pages->isVisible(),
              "choosing page previews keeps the requested panel visible");
        window.signatureAction->trigger();
        settle();
        if (size.width() < 1200)
            check(!dock->isVisible(), "opening settings returns to the compact automatic layout");
        auto control = window.findChild<QToolButton*>("referenceControl");
        check(control && control->isVisible() &&
                  window.rect().contains(QRect(control->mapTo(&window, QPoint()), control->size())),
              "fixed reference entry stays inside the window");
        QTest::mouseClick(control, Qt::LeftButton, Qt::NoModifier, control->rect().center());
        settle();
        check(dock->isVisible(), "main reference button reopens the current tab");
        openReferenceMenu(window, "showBookmarkReferences");
        check(tree->isVisible() && dock->isVisible(),
              "reference menu opens bookmarks with settings");
    }
    check(encodePdf(window.doc.pdf()) == document && window.doc.cursor == cursor &&
              window.doc.dirty() && fileHash(window.doc.source) == hash,
          "reference UI preserves unsaved signature, PDF, Undo and original");
    return {{"cases", cases}, {"signature_draft_preserved", true}, {"native_input", "未実行"}};
}
QJsonObject testRepeatSearchReference(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.activateWindow();
    window.openFile(fixtures + "/viewer-search.pdf");
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    const auto hash = fileHash(window.doc.source);
    auto panel = search(window, "alpha");
    check(panel->session()->rowCount() == 4, "four frozen reference occurrences");
    auto list = panel->findChild<QListView*>("searchResults");
    const auto first = window.canvas->viewState();
    const auto selected = panel->activeMatch();
    window.canvas->scrollBy(QPoint(0, 180));
    settle();
    check(positionError(window, first) > 20, "reader has actually scrolled away from the result");
    clickResult(list, 0);
    ready(window, 0);
    settle();
    const auto clickError = positionError(window, first);
    writeRows(output + "/repeat-search-reference.json",
              QJsonArray{QJsonObject{{"click_error_DIP", clickError}}});
    check(clickError <= 2 && panel->activeMatch() == selected,
          "clicking an already selected result returns to its PDF occurrence");
    window.canvas->scrollBy(QPoint(0, 180));
    settle();
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Return);
    settle();
    check(positionError(window, first) <= 2,
          "Enter on the selected result returns to its occurrence");
    const auto before = window.canvas->viewState();
    clickResult(list, 3);
    ready(window, 2);
    settle();
    clickResult(list, 3);
    window.findChild<QAction*>("previousView")->trigger();
    ready(window, before.anchor.page);
    settle();
    check(positionError(window, before) <= 2,
          "one Back undoes one result navigation without a duplicate history entry");
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Down);
    settle();
    check(panel->session()->indexOf(panel->activeMatch()) == 1,
          "keyboard arrows continue to navigate between occurrences");
    const auto beforePress = window.canvas->viewState();
    const auto activeBeforePress = panel->activeMatch();
    const auto lastIndex = list->model()->index(3, 0);
    list->scrollTo(lastIndex);
    settle();
    const auto lastRect = list->visualRect(lastIndex).intersected(list->viewport()->rect());
    QTest::mousePress(list->viewport(), Qt::LeftButton, Qt::NoModifier, lastRect.center());
    check(panel->activeMatch() == activeBeforePress && positionError(window, beforePress) <= 2,
          "pointer selection does not navigate before the click is confirmed");
    QTest::mouseRelease(list->viewport(), Qt::LeftButton, Qt::NoModifier, lastRect.center());
    ready(window, 2);
    check(panel->session()->indexOf(panel->activeMatch()) == 3,
          "confirmed pointer selection navigates exactly to its occurrence");
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty() &&
              window.doc.cursor == 0 && fileHash(window.doc.source) == hash,
          "repeated result activation and reading history never edit the PDF");
    window.grab().save(output + "/repeat-search-reference.png");
    return {{"selected_click", true},
            {"selected_Enter", true},
            {"one_Back", true},
            {"click_error_DIP", clickError},
            {"native_input", "未実行"}};
}
QJsonObject testSearchResultResize(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/viewer-search.pdf");
    ready(window, 0);
    const auto original = encodePdf(window.doc.pdf());
    auto panel = search(window, "交通費");
    check(panel->session()->rowCount() == 4, "four frozen Japanese reference occurrences");
    auto list = panel->findChild<QListView*>("searchResults");
    auto dock = window.findChild<QDockWidget*>("navigationDock");
    const auto selected = panel->activeMatch();
    QJsonArray rows;
    for (int width : {270, 210, 270, 210})
    {
        window.resizeDocks({dock}, {width}, Qt::Horizontal);
        settle();
        for (int row = 0; row < panel->session()->rowCount(); ++row)
        {
            const auto index = panel->session()->index(row);
            QStyleOptionViewItem option;
            option.initFrom(list);
            option.widget = list;
            option.font = list->font();
            option.fontMetrics = QFontMetrics(option.font);
            option.features |= QStyleOptionViewItem::WrapText;
            option.rect = list->viewport()->rect();
            const auto expected = list->itemDelegate()->sizeHint(option, index);
            const auto actual = list->visualRect(index);
            rows.append(QJsonObject{{"dock_width", dock->width()},
                                    {"viewport_width", list->viewport()->width()},
                                    {"row", row},
                                    {"actual_height", actual.height()},
                                    {"required_height", expected.height()}});
            writeRows(output + "/search-result-resize.json", rows);
            check(actual.height() + 2 >= expected.height(),
                  "search excerpt row relayouts to the height required at the current width");
        }
        check(panel->activeMatch() == selected, "excerpt relayout retains the selected occurrence");
    }
    check(encodePdf(window.doc.pdf()) == original && !window.doc.dirty() && window.doc.cursor == 0,
          "search result relayout never changes the PDF or Undo");
    window.grab().save(output + "/search-result-resize.png");
    return {{"rows", rows}, {"selected_occurrence_preserved", true}};
}
QJsonObject testReferenceDuringOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(800, 480);
    window.show();
    window.activateWindow();
    window.openFile(fixtures + "/D07.pdf");
    ready(window, 0);
    const auto scan = readPdf(fixtures + "/D03.pdf");
    window.doc.commit(insertPages(window.doc.pdf(), scan, {0, 1, 2, 3, 4, 5, 6, 7}, 1));
    window.doc.putSignature(0, "OCR中の署名保持", {60, 50}, 14, Qt::black);
    window.signature->setPlainText("あとで配置する署名");
    window.refresh(true);
    ready(window, 0);
    const auto before = encodePdf(window.doc.pdf());
    const auto revision = window.doc.revision;
    const auto cursor = window.doc.cursor;
    const auto sourceHash = fileHash(window.doc.source);
    const auto scanHash = fileHash(fixtures + "/D03.pdf");
    window.ocrAction->trigger();
    window.language->setCurrentIndex(0);
    window.scope->setCurrentIndex(0);
    QString unexpectedDialog;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout,
                     [&]
                     {
                         if (auto message =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             unexpectedDialog = message->text();
                             message->accept();
                         }
                     });
    dialogs.start(50);
    window.startOcr();
    check(QTest::qWaitFor(
              [&] {
                  return window.worker && window.doc.busy &&
                         window.progress->text().startsWith("OCR 2 /");
              },
              90000),
          "actual OCR reaches the first inserted scan");
    openReferenceMenu(window, "showBookmarkReferences");
    auto tree = window.findChild<QTreeWidget*>("bookmarkTree");
    check(tree->isVisible() && tree->topLevelItemCount() > 0,
          "existing form document bookmarks are available during OCR");
    tree->setCurrentItem(tree->topLevelItem(0));
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Return);
    ready(window, 0);
    openReferenceMenu(window, "showPageReferences");
    window.pages->setCurrentRow(1);
    ready(window, 1);
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Plus, Qt::ControlModifier);
    settle();
    check(window.doc.busy && window.worker && window.canvas->page == 1 &&
              window.findChild<QAction*>("previousView")->isEnabled(),
          "OCR permits reference navigation, zoom and reading history");
    check(window.cancel->isVisible() &&
              window.rect().contains(
                  QRect(window.cancel->mapTo(&window, QPoint()), window.cancel->size())),
          "reference controls never push OCR cancellation outside the compact window");
    window.grab().save(output + "/reference-during-ocr.png");
    window.cancel->setFocus();
    QTest::keyClick(window.cancel, Qt::Key_Space);
    check(QTest::qWaitFor([&] { return !window.worker; }, 10000), "OCR cancellation finishes");
    dialogs.stop();
    check(unexpectedDialog.isEmpty(),
          "reference workload does not unexpectedly complete or fail OCR");
    check(encodePdf(window.doc.pdf()) == before && window.doc.revision == revision &&
              window.doc.cursor == cursor && window.doc.dirty() && window.doc.pages() == 9 &&
              fileHash(window.doc.source) == sourceHash &&
              fileHash(fixtures + "/D03.pdf") == scanHash &&
              window.signature->toPlainText() == "あとで配置する署名",
          "OCR cancellation preserves forms, annotations, signature, draft, history and both "
          "sources");
    return {{"actual_OCR", true},
            {"reference_menu_during_OCR", true},
            {"cancellation_preserves_document", true},
            {"pages", 9},
            {"native_input", "未実行; QtTest events"}};
}
QJsonObject testReferenceContexts(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(800, 480);
    window.show();
    window.activateWindow();
    auto control = window.findChild<QToolButton*>("referenceControl");
    check(control && !control->isEnabled(), "reference entry is disabled without a document");
    window.openFile(fixtures + "/D01.pdf");
    ready(window, 0);
    openReferenceMenu(window, "showBookmarkReferences");
    auto tabs = window.findChild<QTabWidget*>("navigationTabs");
    auto tree = window.findChild<QTreeWidget*>("bookmarkTree");
    check(tree->topLevelItemCount() == 0 && !tree->isVisible() && tabs->tabBar()->hasFocus(),
          "a PDF without bookmarks leaves keyboard focus on the visible reference tabs");
    openReferenceMenu(window, "showSearchReferences");
    check(window.query->hasFocus(), "search menu focuses the actual query editor");
    window.doc.open(fixtures + "/viewer-navigation-restricted.pdf", "navigation-user");
    window.refresh(true);
    ready(window, 0);
    const auto before = window.doc.pdf();
    const auto hash = fileHash(window.doc.source);
    check(!window.doc.copyAllowed && !window.doc.readOnly.isEmpty() &&
              !window.signatureAction->isEnabled() && control->isEnabled(),
          "protected PDF enables references while editing stays disabled");
    openReferenceMenu(window, "showBookmarkReferences");
    QTreeWidgetItem* destination = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == "UserUnit 2")
            destination = *it;
    check(destination, "protected PDF retains its real outline");
    for (auto parent = destination->parent(); parent; parent = parent->parent())
        parent->setExpanded(true);
    tree->setCurrentItem(destination);
    tree->scrollToItem(destination);
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Return);
    ready(window, 5);
    window.findChild<QAction*>("previousView")->trigger();
    settle();
    check(window.doc.pdf() == before && !window.doc.dirty() && window.doc.cursor == 0 &&
              fileHash(window.doc.source) == hash && !window.doc.copyAllowed &&
              !window.doc.readOnly.isEmpty(),
          "protected reference navigation never modifies or decrypts the saved source");
    window.grab().save(output + "/protected-reference.png");
    return {{"no_document_disabled", true},
            {"empty_bookmark_focus", true},
            {"protected_reading", true},
            {"original_unchanged", true}};
}
} // namespace tatsu
