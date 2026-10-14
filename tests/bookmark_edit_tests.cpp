#include "bookmark_edit_tests.h"
#include "bookmark_edit_dialog.h"
#include "form_fields.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
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
template <typename F> QString rejects(F operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
    fail("Invalid bookmark operation succeeded");
}
void unchangedPages(const PDFDocument& before, const PDFDocument& after)
{
    check(before.getCatalog()->getPageCount() == after.getCatalog()->getPageCount(),
          "No pages deleted");
    for (int i = 0; i < int(before.getCatalog()->getPageCount()); ++i)
    {
        const auto ref = before.getCatalog()->getPage(i)->getPageReference();
        check(before.getObjectByReference(ref) == after.getObjectByReference(ref),
              "Whole page dictionary and references unchanged");
    }
}
QVector<BookmarkEntry> many(int count)
{
    QVector<BookmarkEntry> entries;
    for (int i = 0; i < count; ++i)
        entries << BookmarkEntry{{}, QString("しおり %1").arg(i + 1), -1, 0, true, true};
    return entries;
}
} // namespace
QJsonObject testBookmarkLifecycle(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/viewer-navigation.pdf");
    const auto hash = fileHash(document.source);
    const auto original = editableBookmarks(document.pdf());
    check(!original.isEmpty(), "Existing frozen navigation outlines available");
    PDFDocumentBuilder seed(&document.pdf());
    auto first = *seed.getObjectByReference(original.front().source).getDictionary();
    detail::set(first, "C",
                detail::arrObject({detail::number(0), detail::number(.2), detail::number(.7)}));
    detail::set(first, "F", PDFObject::createInteger(3));
    detail::set(first, "TatsujinTestProperty",
                PDFObject::createString("preserve-unknown-property"));
    seed.setObject(original.front().source, detail::dictObject(first));
    document.commit(seed.build());
    document.putSignature(0, "しおり編集で保持", {80, 300}, 12, Qt::black);
    const auto before = document.pdf();
    writeCandidate(before, output + "/bookmarks-before.pdf");
    auto old = editableBookmarks(before);
    QVector<BookmarkEntry> edited;
    old[0].title = "日本語の見出し";
    old[0].expanded = false;
    edited << old[0] << BookmarkEntry{{}, "追加した子", 0, 2, true, true};
    for (int i = 1; i < old.size(); ++i)
    {
        auto item = old[i];
        if (item.parent >= 1)
            ++item.parent;
        edited << item;
    }
    edited << BookmarkEntry{{},   "最後の章", -1, int(before.getCatalog()->getPageCount()) - 1,
                            true, true};
    auto candidate = replaceBookmarks(before, edited);
    unchangedPages(before, candidate);
    for (const auto& item : old)
    {
        const auto a = before.getObjectByReference(item.source).getDictionary();
        const auto b = candidate.getObjectByReference(item.source).getDictionary();
        for (const char* key : {"Dest", "A", "C", "F", "TatsujinTestProperty"})
            check(a->get(key) == b->get(key), QString("Existing %1 unchanged").arg(key));
    }
    const int cursor = document.cursor;
    document.commit(candidate);
    check(document.cursor == cursor + 1, "All outline changes committed once");
    document.undo();
    check(document.pdf() == before, "One Undo restores exact old outlines");
    document.redo();
    document.save(output + "/bookmarks-edited.pdf");
    Document reopened;
    reopened.open(document.target);
    auto saved = editableBookmarks(reopened.pdf());
    check(saved.size() == old.size() + 2 && saved[0].title == "日本語の見出し" &&
              !saved[0].expanded && saved[1].parent == 0,
          "Saved Unicode, hierarchy and collapsed state reopen");
    saved[0].changeDestination = true;
    saved[0].page = int(before.getCatalog()->getPageCount()) - 1;
    reopened.commit(replaceBookmarks(reopened.pdf(), saved));
    reopened.save(output + "/bookmarks-retargeted.pdf");
    check(editableBookmarks(reopened.pdf())[0].page == saved[0].page,
          "Explicit retarget uses requested physical page");
    const auto retargeted = reopened.pdf();
    reopened.commit(replaceBookmarks(reopened.pdf(), {}));
    check(editableBookmarks(reopened.pdf()).isEmpty(), "All outlines can be removed");
    unchangedPages(retargeted, reopened.pdf());
    reopened.save(output + "/bookmarks-empty.pdf");
    reopened.undo();
    check(reopened.pdf() == retargeted, "Undo restores removed outline hierarchy");
    const auto empty = readPdf(fixtures + "/D02.pdf");
    check(replaceBookmarks(empty, {}) == empty,
          "Empty replacement does not add phantom outline objects");
    Document form;
    form.open(fixtures + "/D07.pdf");
    auto fields = formFields(form.pdf());
    auto field = std::find_if(fields.begin(), fields.end(),
                              [](const auto& value) { return value.name == "name"; });
    check(field != fields.end(), "Japanese form found");
    putFormValue(form, field->widget, {"髙橋 香織"});
    writeCandidate(form.pdf(), output + "/bookmarks-form-before.pdf");
    auto entries = editableBookmarks(form.pdf());
    entries[0].title = "申込書の入力";
    auto changed = replaceBookmarks(form.pdf(), entries);
    unchangedPages(form.pdf(), changed);
    form.commit(changed);
    form.save(output + "/bookmarks-form.pdf");
    auto scan = readPdf(fixtures + "/D06.pdf");
    const auto copied = pageText(scan, 0);
    check(!copied.isEmpty(), "Existing OCR text is available before outline editing");
    auto scanEdited = replaceBookmarks(scan, many(1));
    unchangedPages(scan, scanEdited);
    check(pageText(scanEdited, 0) == copied, "Existing OCR text is preserved after editing");
    writeCandidate(scan, output + "/bookmarks-ocr-before.pdf");
    writeCandidate(scanEdited, output + "/bookmarks-ocr.pdf");
    auto nested = many(32);
    for (int i = 0; i < nested.size(); ++i)
        nested[i].parent = i - 1;
    auto deepest = replaceBookmarks(readPdf(fixtures + "/D02.pdf"), nested);
    check(editableBookmarks(deepest).size() == 32, "32-level boundary succeeds without truncation");
    writeCandidate(deepest, output + "/bookmarks-depth32.pdf");
    check(fileHash(document.source) == hash, "Frozen original stays unchanged");
    return {{"original_items", old.size()},
            {"saved_items", edited.size()},
            {"destinations_and_unknown_properties_preserved", true},
            {"saved_Unicode_hierarchy_reedit_remove_Undo", true},
            {"form_signature_pages_preserved", true},
            {"existing_OCR_preserved", true},
            {"depth_boundary", 32}};
}
QJsonObject testBookmarkFailures(const QString& fixtures)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    const auto bytes = encodePdf(source);
    QJsonArray cases;
    auto invalid = [&](QString name, const std::function<void()>& operation)
    {
        cases.append(QJsonObject{{"case", name}, {"error", rejects(operation)}});
        check(encodePdf(source) == bytes, "Failure keeps original bytes");
    };
    for (const auto& text : {QString{}, QString(201, 'x'), QString("name\nline"), QString(QChar(0)),
                             QString(QChar(0x2028))})
        invalid("invalid title",
                [&]
                {
                    auto entries = many(1);
                    entries[0].title = text;
                    replaceBookmarks(source, entries);
                });
    for (int page : {-1, int(source.getCatalog()->getPageCount())})
        invalid("invalid destination",
                [&]
                {
                    auto entries = many(1);
                    entries[0].page = page;
                    replaceBookmarks(source, entries);
                });
    invalid("missing new destination",
            [&]
            {
                auto entries = many(1);
                entries[0].changeDestination = false;
                replaceBookmarks(source, entries);
            });
    invalid("unknown source",
            [&]
            {
                auto entries = many(1);
                entries[0].source = {999999, 0};
                replaceBookmarks(source, entries);
            });
    invalid("self parent",
            [&]
            {
                auto entries = many(1);
                entries[0].parent = 0;
                replaceBookmarks(source, entries);
            });
    invalid("closed parent branch",
            [&]
            {
                auto entries = many(3);
                entries[2].parent = 0;
                replaceBookmarks(source, entries);
            });
    invalid("1001 entries", [&] { replaceBookmarks(source, many(1001)); });
    invalid("33 levels",
            [&]
            {
                auto entries = many(33);
                for (int i = 0; i < entries.size(); ++i)
                    entries[i].parent = i - 1;
                replaceBookmarks(source, entries);
            });
    invalid("empty document", [&] { replaceBookmarks(PDFDocument{}, many(1)); });
    invalid("signed document",
            [&] { replaceBookmarks(readPdf(fixtures + "/D08-signed.pdf"), many(1)); });
    auto nav = readPdf(fixtures + "/viewer-navigation.pdf");
    const auto old = editableBookmarks(nav);
    invalid("duplicate source",
            [&]
            {
                auto entries = old;
                auto copy = old[0];
                copy.parent = -1;
                entries << copy;
                replaceBookmarks(nav, entries);
            });
    auto malformed = [&](const char* key, PDFObject value)
    {
        PDFDocumentBuilder builder(&nav);
        auto dictionary = *builder.getObjectByReference(old[0].source).getDictionary();
        detail::set(dictionary, key, value);
        builder.setObject(old[0].source, detail::dictObject(dictionary));
        return builder.build();
    };
    auto cyclic = malformed("Next", PDFObject::createReference(old[0].source));
    invalid("cyclic input outline", [&] { editableBookmarks(cyclic); });
    auto broken = malformed("Parent", PDFObject::createReference(old.back().source));
    invalid("broken input parent", [&] { editableBookmarks(broken); });
    int checks = 0;
    invalid("cancel while rewriting actual dictionaries",
            [&] { replaceBookmarks(source, many(1000), [&] { return ++checks >= 1500; }); });
    check(checks == 1500,
          "Cancellation reached dictionary-writing phase after validating all entries");
    return {{"rejected", cases}, {"cancel_checks", checks}, {"originals_preserved", true}};
}
QJsonObject testBookmarkEditUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(800, 480);
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "Initial page ready");
    auto browser = window.findChild<QTreeWidget*>("bookmarkTree");
    window.findChild<QTabWidget*>("navigationTabs")->setCurrentWidget(browser->parentWidget());
    auto edit = window.findChild<QPushButton*>("editDocumentBookmarks");
    check(edit && edit->isEnabled(), "Edit available for PDF without existing outlines");
    QString error;
    int mode = 0, ticks = 0, busyTicks = 0;
    auto operate = [&]
    {
        int state = 0;
        QElapsedTimer elapsed;
        elapsed.start();
        QTimer timer;
        QObject::connect(
            &timer, &QTimer::timeout,
            [&]
            {
                ++ticks;
                if (auto message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                {
                    error = message->text();
                    message->accept();
                    return;
                }
                auto dialog = dynamic_cast<BookmarkEditDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                for (auto worker : dialog->findChildren<QThread*>())
                    if (worker->isRunning())
                        ++busyTicks;
                if (elapsed.elapsed() > 20000)
                {
                    error = "Bookmark UI deadline";
                    dialog->reject();
                    return;
                }
                try
                {
                    auto tree = dialog->findChild<QTreeWidget*>("bookmarkEditTree");
                    auto title = dialog->findChild<QLineEdit*>("bookmarkTitle");
                    auto click = [&](const char* name)
                    { QTest::mouseClick(dialog->findChild<QPushButton*>(name), Qt::LeftButton); };
                    auto destination = dialog->findChild<QSpinBox*>("bookmarkDestinationPage");
                    if (state == 0)
                    {
                        dialog->resize(760, 480);
                        if (mode == 0)
                        {
                            click("bookmarkAdd");
                            title->setText("第一章");
                            destination->setValue(1);
                            click("bookmarkAdd");
                            title->setText("付録");
                            destination->setValue(4);
                            click("bookmarkIndent");
                            click("bookmarkAdd");
                            title->setText("第二節");
                            destination->setValue(2);
                            click("bookmarkOutdent");
                            click("bookmarkUp");
                            title->clear();
                            click("bookmarkApply");
                            state = 1;
                        }
                        else if (mode == 1)
                        {
                            title->setText("保存後の章名変更");
                            dialog->findChild<QCheckBox*>("bookmarkChangeDestination")
                                ->setChecked(true);
                            destination->setValue(3);
                            state = 2;
                        }
                        else if (mode == 2)
                        {
                            tree->setCurrentItem(tree->topLevelItem(1));
                            check(dialog->findChild<QPushButton*>("bookmarkDelete")
                                      ->text()
                                      .contains("2項目"),
                                  "Delete explicitly includes child count");
                            click("bookmarkDelete");
                            state = 2;
                        }
                        else
                        {
                            title->setText("取消する候補");
                            dialog->reject();
                            state = 3;
                        }
                    }
                    else if (state == 1 && dialog->findChild<QLabel*>("bookmarkEditMessage")
                                               ->text()
                                               .contains("1〜200"))
                    {
                        title->setText("第二節");
                        state = 2;
                    }
                    else if (state == 2 &&
                             dialog->findChild<QPushButton*>("bookmarkApply")->isEnabled())
                    {
                        dialog->grab().save(output + QString("/bookmark-edit-ui-%1.png").arg(mode));
                        click("bookmarkApply");
                        state = 3;
                    }
                }
                catch (const std::exception& failure)
                {
                    error = QString::fromUtf8(failure.what());
                    dialog->reject();
                }
            });
        timer.start(1);
        QTest::mouseClick(edit, Qt::LeftButton);
        timer.stop();
        check(error.isEmpty(), error);
        check(state == 3, "Actual edit dialog completed");
    };
    const int cursor = window.doc.cursor;
    operate();
    check(window.doc.cursor == cursor + 1, "Product applies entire edited tree once");
    auto navigate = [&](const QString& name, int page)
    {
        QTreeWidgetItem* found = nullptr;
        for (QTreeWidgetItemIterator i(browser); *i; ++i)
            if ((*i)->text(0) == name)
                found = *i;
        check(found, "Saved bookmark appears in reader");
        if (found->parent())
            found->parent()->setExpanded(true);
        browser->scrollToItem(found);
        QCoreApplication::processEvents();
        QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier,
                          browser->visualItemRect(found).center());
        check(QTest::qWaitFor(
                  [&] { return window.canvas->page == page && window.canvas->pageReady(page); },
                  15000),
              "Actual saved bookmark moves viewer");
    };
    navigate("第二節", 1);
    navigate("付録", 3);
    window.doc.save(output + "/bookmarks-ui.pdf");
    window.openFile(window.doc.target);
    mode = 1;
    operate();
    navigate("保存後の章名変更", 2);
    window.doc.save(output + "/bookmarks-ui-reedited.pdf");
    const auto edited = window.doc.pdf();
    mode = 2;
    operate();
    check(editableBookmarks(window.doc.pdf()).size() == 1, "Delete removes selected subtree only");
    window.undoAction->trigger();
    check(window.doc.pdf() == edited, "Product Undo restores deleted subtree");
    const int beforeCancel = window.doc.cursor;
    mode = 3;
    operate();
    check(window.doc.pdf() == edited && window.doc.cursor == beforeCancel,
          "Cancel keeps document and Undo");
    return {{"actual_create_reedit_subtree_delete_Undo_cancel", true},
            {"saved_navigation_pages", QJsonArray{2, 4, 3}},
            {"GUI_event_ticks", ticks},
            {"worker_busy_ticks", busyTicks},
            {"native_UI", "未実行"}};
}
QJsonObject testBookmarkCancel(const QString& fixtures)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    auto seeded = replaceBookmarks(source, many(1000));
    const auto bytes = encodePdf(seeded);
    BookmarkEditDialog dialog(seeded, 0);
    dialog.show();
    dialog.findChild<QLineEdit*>("bookmarkTitle")->setText("取消対象");
    QTest::mouseClick(dialog.findChild<QPushButton*>("bookmarkApply"), Qt::LeftButton);
    check(!dialog.findChild<QPushButton*>("bookmarkApply")->isEnabled(),
          "Actual apply starts owned worker");
    QTest::mouseClick(dialog.findChild<QPushButton*>("bookmarkCancel"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return !dialog.isVisible(); }, 15000),
          "Cancel joins worker and closes");
    check(dialog.result() != QDialog::Accepted && encodePdf(seeded) == bytes,
          "Cancel publishes no partial outline tree");
    rejects([&] { dialog.takeDocument(); });
    return {{"items", 1000}, {"owned_worker_cancel_no_publish", true}};
}
} // namespace tatsu
