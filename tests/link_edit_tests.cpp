#include "link_edit_tests.h"
#include "form_fields.h"
#include "link_edit_dialog.h"
#include "page_operations.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "search_panel.h"
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
    fail("Invalid link operation succeeded");
}
PDFObject annotation(const PDFDocument& document, const LinkEntry& entry)
{
    const auto page = document.getObjectByReference(
        document.getCatalog()->getPage(entry.page)->getPageReference());
    const auto list = document.getObject(page.getDictionary()->get("Annots"));
    return list.getArray()->getItem(entry.sourceIndex);
}
void sameBody(const PDFDocument& before, const PDFDocument& after)
{
    check(before.getCatalog()->getPageCount() == after.getCatalog()->getPageCount(),
          "Pages retained");
    for (int i = 0; i < int(before.getCatalog()->getPageCount()); ++i)
    {
        const auto ref = before.getCatalog()->getPage(i)->getPageReference();
        auto a = *before.getObjectByReference(ref).getDictionary();
        auto b = *after.getObjectByReference(ref).getDictionary();
        a.removeEntry("Annots");
        b.removeEntry("Annots");
        check(detail::dictObject(a) == detail::dictObject(b),
              "Page body/resources/boxes unchanged");
    }
}
QVector<LinkEntry> many(int count)
{
    QVector<LinkEntry> result;
    for (int i = 0; i < count; ++i)
        result << LinkEntry{0, -1, {10, 10, 50, 20}, QString("リンク%1").arg(i), LinkTarget::Page,
                            0, {}};
    return result;
}
void drag(LinkPreview* preview, QPointF from, QPointF to)
{
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier,
                      preview->physicalToWidget(from).toPoint());
    QTest::mouseMove(preview, preview->physicalToWidget(to).toPoint(), 40);
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier,
                        preview->physicalToWidget(to).toPoint());
}
} // namespace
QJsonObject testLinkLifecycle(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    const auto hash = fileHash(document.source);
    PDFDocumentBuilder geometry(&document.pdf());
    for (int i = 0; i < document.pages(); ++i)
    {
        const auto ref = document.pdf().getCatalog()->getPage(i)->getPageReference();
        auto dictionary = *geometry.getObjectByReference(ref).getDictionary();
        detail::set(dictionary, "UserUnit", detail::number(1.5));
        detail::set(dictionary, "CropBox", detail::rectObject({20, 30, 300, 400}));
        geometry.setObject(ref, detail::dictObject(dictionary));
    }
    document.commit(geometry.build());
    document.putSignature(0, "リンクと保持", {100, 200}, 12, Qt::black);
    const auto before = document.pdf();
    writeCandidate(before, output + "/links-before.pdf");
    auto entries = editableLinks(before);
    for (int page = 0; page < document.pages(); ++page)
        entries << LinkEntry{page,
                             -1,
                             {15, 25, 90, 35},
                             QString("日本語のリンク %1").arg(page + 1),
                             LinkTarget::Page,
                             (page + 1) % document.pages(),
                             {}};
    entries << LinkEntry{0,
                         -1,
                         {120, 110, 80, 20},
                         "公開サイト",
                         LinkTarget::Web,
                         0,
                         "https://example.com/資料?q=日本語#section"};
    auto candidate = replaceLinks(before, entries);
    sameBody(before, candidate);
    const int cursor = document.cursor;
    document.commit(candidate);
    check(document.cursor == cursor + 1, "All link changes are one transaction");
    document.undo();
    check(document.pdf() == before, "One Undo restores original document");
    document.redo();
    document.save(output + "/links-edited.pdf");
    Document reopened;
    reopened.open(document.target);
    auto saved = editableLinks(reopened.pdf());
    check(saved.size() == entries.size() && saved[0].description == entries[0].description,
          "Saved standard links reopen");
    check(replaceLinks(reopened.pdf(), saved) == reopened.pdf(),
          "Unchanged candidate is a true no-op");
    saved[0].rectangle.translate(20, 30);
    saved[0].rectangle.setSize({110, 45});
    saved[0].target = LinkTarget::Page;
    saved[0].destination = document.pages() - 1;
    saved[0].description = "保存後に範囲と宛先を変更";
    saved.removeLast();
    reopened.commit(replaceLinks(reopened.pdf(), saved));
    reopened.save(output + "/links-reedited.pdf");
    check(signatures(reopened.pdf(), 0).size() == 1, "Reeditable Japanese signature retained");
    check(fileHash(document.source) == hash, "Original is byte-identical");
    auto existing = readPdf(fixtures + "/viewer-navigation.pdf");
    auto old = editableLinks(existing);
    check(!old.isEmpty(), "Existing navigation links available");
    PDFDocumentBuilder seed(&existing);
    const auto ref = annotation(existing, old[0]).getReference();
    auto dictionary = *seed.getObjectByReference(ref).getDictionary();
    const auto box = pageMatrix(existing.getCatalog()->getPage(old[0].page))
                         .inverted()
                         .mapRect(old[0].rectangle);
    detail::set(dictionary, "QuadPoints",
                detail::arrObject({detail::number(box.left()), detail::number(box.bottom()),
                                   detail::number(box.right()), detail::number(box.bottom()),
                                   detail::number(box.left()), detail::number(box.top()),
                                   detail::number(box.right()), detail::number(box.top())}));
    detail::set(dictionary, "TatsujinTestProperty",
                PDFObject::createString("keep-unknown-link-property"));
    seed.setObject(ref, detail::dictObject(dictionary));
    existing = seed.build();
    writeCandidate(existing, output + "/links-existing-before.pdf");
    old = editableLinks(existing);
    auto edited = old;
    edited[0].rectangle = {70, 100, 180, 45};
    edited[0].description = "既存リンクの日本語説明";
    auto updated = replaceLinks(existing, edited);
    sameBody(existing, updated);
    for (int i = 0; i < old.size(); ++i)
    {
        const auto a = existing.getObject(annotation(existing, old[i])).getDictionary();
        const auto b =
            updated.getObject(annotation(updated, editableLinks(updated)[i])).getDictionary();
        for (auto key : {"A", "Dest", "AP", "C", "Border", "F", "TatsujinTestProperty"})
            check(a->get(key) == b->get(key), QString("Existing link %1 preserved").arg(key));
    }
    writeCandidate(updated, output + "/links-existing-edited.pdf");
    // Direct Link dictionaries are legal and must remain editable.
    PDFDocumentBuilder direct(&existing);
    const auto pageRef = existing.getCatalog()->getPage(old[0].page)->getPageReference();
    auto pageDictionary = *direct.getObjectByReference(pageRef).getDictionary();
    auto list = existing.getObject(pageDictionary.get("Annots"));
    std::vector<PDFObject> items;
    for (size_t i = 0; i < list.getArray()->getCount(); ++i)
        items.push_back(int(i) == old[0].sourceIndex
                            ? existing.getObject(list.getArray()->getItem(i))
                            : list.getArray()->getItem(i));
    detail::set(pageDictionary, "Annots", detail::arrObject(items));
    direct.setObject(pageRef, detail::dictObject(pageDictionary));
    const auto directSource = direct.build();
    auto directValues = editableLinks(directSource);
    directValues[0].description = "直接辞書の編集";
    auto directResult = replaceLinks(directSource, directValues);
    writeCandidate(directSource, output + "/links-direct-before.pdf");
    writeCandidate(directResult, output + "/links-direct-edited.pdf");
    Document form;
    form.open(fixtures + "/D07.pdf");
    for (const auto& field : formFields(form.pdf()))
        if (field.name == "name")
            putFormValue(form, field.widget, {"髙橋 香織"});
    writeCandidate(form.pdf(), output + "/links-form-before.pdf");
    auto formValues = editableLinks(form.pdf());
    formValues << LinkEntry{0, -1, {80, 350, 100, 35}, "フォームのリンク", LinkTarget::Page, 0, {}};
    form.commit(replaceLinks(form.pdf(), formValues));
    form.save(output + "/links-form-edited.pdf");
    check(formFields(form.pdf()).size() == formFields(readPdf(fixtures + "/D07.pdf")).size(),
          "All form widgets retained");
    return {{"rotations", QJsonArray{0, 90, 180, 270}},
            {"UserUnit", 1.5},
            {"existing_links", old.size()},
            {"direct_dictionary", true},
            {"single_Undo_reedit_delete", true},
            {"original_unchanged", true}};
}
QJsonObject testLinkFailures(const QString& fixtures)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    const auto bytes = encodePdf(source);
    QJsonArray failures;
    auto invalid = [&](QString name, const std::function<void()>& call)
    {
        failures << QJsonObject{{"case", name}, {"error", rejects(call)}};
        check(encodePdf(source) == bytes, "Failure leaves source unchanged");
    };
    for (auto url :
         {"", "file:///C:/test.pdf", "javascript:alert(1)", "ftp://example.com", "https://",
          "https://user:secret@example.com", " https://example.com", "https://example.com/a b",
          "https://example.com/\n", "https://example.com/%zz"})
        invalid("invalid URI", [&, url] { normalizedLinkUrl(url); });
    invalid("URI too long",
            [&] { normalizedLinkUrl("https://example.com/" + QString(4096, 'a')); });
    auto valid = many(1);
    auto mutate = [&](QString name, const std::function<void(LinkEntry&)>& change)
    {
        invalid(name,
                [&]
                {
                    auto entries = valid;
                    change(entries[0]);
                    replaceLinks(source, entries);
                });
    };
    mutate("missing new target", [](auto& entry) { entry.target = LinkTarget::Preserve; });
    mutate("invalid source", [](auto& entry) { entry.sourceIndex = 10000; });
    mutate("invalid placement page", [](auto& entry) { entry.page = -1; });
    mutate("invalid destination page", [](auto& entry) { entry.destination = 9999; });
    mutate("unsupported target", [](auto& entry) { entry.target = LinkTarget(99); });
    mutate("zero width", [](auto& entry) { entry.rectangle.setWidth(0); });
    mutate("off page", [](auto& entry) { entry.rectangle.moveLeft(-1); });
    mutate("off right", [](auto& entry) { entry.rectangle.setWidth(100000); });
    mutate("NaN",
           [](auto& entry) { entry.rectangle.moveTop(std::numeric_limits<double>::quiet_NaN()); });
    mutate("multiline description", [](auto& entry) { entry.description = "第一\n第二"; });
    mutate("long description", [](auto& entry) { entry.description = QString(501, 'a'); });
    invalid("1001 items", [&] { replaceLinks(source, many(1001)); });
    invalid("empty document", [&] { replaceLinks(PDFDocument{}, valid); });
    invalid("signed PDF", [&] { replaceLinks(readPdf(fixtures + "/D08-signed.pdf"), valid); });
    auto seeded = replaceLinks(source, many(1000));
    auto edited = editableLinks(seeded);
    auto duplicates = edited;
    duplicates[1] = duplicates[0];
    invalid("duplicate source", [&] { replaceLinks(seeded, duplicates); });
    PDFDocumentBuilder builder(&seeded);
    auto entry = *seeded.getObject(annotation(seeded, edited[0])).getDictionary();
    detail::set(entry, "QuadPoints", detail::arrObject({detail::number(1)}));
    builder.setObject(annotation(seeded, edited[0]).getReference(), detail::dictObject(entry));
    auto malformed = builder.build();
    auto geometry = editableLinks(malformed);
    geometry[0].rectangle.translate(5, 5);
    invalid("malformed quads", [&] { replaceLinks(malformed, geometry); });
    check(replaceLinks(malformed, editableLinks(malformed)) == malformed,
          "Unchanged malformed quad preserved");
    for (auto& item : edited)
        item.description = "変更候補";
    int checks = 0;
    invalid("cancel after real rewrites",
            [&] { replaceLinks(seeded, edited, [&] { return ++checks == 1500; }); });
    check(checks == 1500 && editableLinks(seeded)[0].description != "変更候補",
          "Cancellation after validation and real rewrites leaves original");
    LinkEditDialog dialog(seeded, 0);
    dialog.show();
    dialog.findChild<QLineEdit*>("linkDescription")->setText("取消する候補");
    QTest::mouseClick(dialog.findChild<QPushButton*>("linkApply"), Qt::LeftButton);
    QTest::mouseClick(dialog.findChild<QPushButton*>("linkCancel"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return !dialog.isVisible(); }, 15000) &&
              dialog.result() != QDialog::Accepted,
          "Actual worker cancel closes without publish");
    rejects([&] { dialog.takeDocument(); });
    return {{"rejected", failures},
            {"worker_cancel_1000_items", true},
            {"cancellation_checks", checks}};
}
QJsonObject testLinkUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.openFile(fixtures + "/D02.pdf");
    window.resize(1000, 700);
    window.show();
    QString error;
    int mode = 0, ticks = 0;
    auto operate = [&]
    {
        int state = 0;
        QElapsedTimer deadline;
        deadline.start();
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(
            &timer, &QTimer::timeout,
            [&]
            {
                ++ticks;
                auto dialog = dynamic_cast<LinkEditDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                if (deadline.elapsed() > 25000)
                {
                    error = "Link UI deadline";
                    dialog->reject();
                    return;
                }
                auto preview =
                    dynamic_cast<LinkPreview*>(dialog->findChild<QWidget*>("linkPreview"));
                auto page = dialog->findChild<QComboBox*>("linkEditPage");
                auto click = [&](const char* name)
                { QTest::mouseClick(dialog->findChild<QPushButton*>(name), Qt::LeftButton); };
                try
                {
                    if (state == 0 && !preview->image.isNull())
                    {
                        dialog->resize(760, 480);
                        if (mode == 0)
                        {
                            page->setCurrentIndex(3);
                            state = 1;
                        }
                        else if (mode == 1)
                        {
                            dialog->findChild<QLineEdit*>("linkDescription")
                                ->setText("保存後にリンクを再編集");
                            dialog->findChild<QSpinBox*>("linkDestination")->setValue(4);
                            dialog->findChild<QComboBox*>("linkTarget")->setCurrentIndex(1);
                            state = 4;
                        }
                        else if (mode == 2)
                        {
                            click("linkDelete");
                            state = 4;
                        }
                        else
                        {
                            dialog->findChild<QLineEdit*>("linkDescription")
                                ->setText("取り消す編集");
                            dialog->reject();
                            state = 5;
                        }
                    }
                    else if (state == 1 && !preview->image.isNull() &&
                             preview->property("shownPage").toInt() == 3)
                    {
                        page->setCurrentIndex(0);
                        state = 2;
                    }
                    else if (state == 2 && !preview->image.isNull() &&
                             preview->property("shownPage").toInt() == 0)
                    {
                        click("linkDraw");
                        drag(preview, {80, 200}, {200, 240});
                        check(preview->regions.size() == 1, "Real drawing creates link candidate");
                        auto box = preview->regions[0].second;
                        drag(preview, box.center(), box.center() + QPointF(25, 20));
                        check(preview->regions[0].second.x() > box.x() + 15,
                              "Actual drag moves rectangle");
                        box = preview->regions[0].second;
                        drag(preview, box.bottomRight(), box.bottomRight() + QPointF(30, 20));
                        check(preview->regions[0].second.width() > box.width() + 15,
                              "Actual corner resizes rectangle");
                        dialog->findChild<QLineEdit*>("linkDescription")
                            ->setText("日本語のページリンク");
                        dialog->findChild<QSpinBox*>("linkDestination")->setValue(2);
                        dialog->findChild<QComboBox*>("linkTarget")->setCurrentIndex(2);
                        dialog->findChild<QLineEdit*>("linkUrl")->setText("javascript:alert(1)");
                        click("linkApply");
                        state = 3;
                    }
                    else if (state == 3 &&
                             dialog->findChild<QPushButton*>("linkApply")->isEnabled() &&
                             dialog->findChild<QLabel*>("linkMessage")->text().contains("http"))
                    {
                        dialog->findChild<QComboBox*>("linkTarget")->setCurrentIndex(1);
                        state = 4;
                    }
                    else if (state == 4)
                    {
                        dialog->grab().save(output + QString("/links-ui-%1.png").arg(mode));
                        check(dialog->findChild<QPushButton*>("linkApply")
                                      ->visibleRegion()
                                      .boundingRect()
                                      .height() > 0,
                              "Apply visible at compact dimensions");
                        click("linkApply");
                        state = 5;
                    }
                }
                catch (const std::exception& e)
                {
                    error = QString::fromUtf8(e.what());
                    dialog->reject();
                }
            });
        timer.start(8);
        const int before = window.doc.cursor;
        window.editLinks();
        timer.stop();
        check(error.isEmpty(), error);
        check(window.doc.cursor == before + (mode == 3 ? 0 : 1),
              "Window makes one commit, cancel makes none");
    };
    operate();
    auto saved = editableLinks(window.doc.pdf());
    check(saved.size() == 1, "UI applies single standard link");
    window.doc.save(output + "/links-ui.pdf");
    window.openFile(window.doc.target);
    auto follow = [&](int expected)
    {
        window.canvas->goToPage(0);
        check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
              "Viewer page ready");
        const auto item = editableLinks(window.doc.pdf())[0];
        const auto point = pageMatrix(window.doc.pdf().getCatalog()->getPage(0))
                               .inverted()
                               .map(item.rectangle.center());
        QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas->pdfToViewport(0, point).toPoint());
        check(QTest::qWaitFor(
                  [&]
                  { return window.canvas->page == expected && window.canvas->pageReady(expected); },
                  15000),
              "Saved link actually navigates main viewer");
    };
    follow(1);
    window.canvas->goToPage(0);
    mode = 1;
    operate();
    follow(3);
    window.doc.save(output + "/links-ui-reedited.pdf");
    window.canvas->goToPage(0);
    const auto edited = window.doc.pdf();
    mode = 2;
    operate();
    check(editableLinks(window.doc.pdf()).isEmpty(), "Delete link applies");
    window.undoAction->trigger();
    check(window.doc.pdf() == edited, "Product Undo restores deleted link");
    mode = 3;
    operate();
    check(window.doc.pdf() == edited, "Cancel keeps exact original candidate");
    return {{"real_draw_move_resize_save_reedit_navigation_delete_Undo_cancel", true},
            {"saved_navigation_pages", QJsonArray{2, 4}},
            {"GUI_event_ticks", ticks},
            {"native_UI", "未実行"}};
}
QJsonObject testLinkOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.openFile(fixtures + "/D03.pdf");
    auto candidate = selectPages(window.doc.pdf(), {2, 6});
    window.doc.commit(candidate);
    window.doc.source.clear();
    window.doc.sourceHash.clear();
    window.doc.putSignature(0, "リンクとOCR", {50, 50}, 12, Qt::black);
    auto links = editableLinks(window.doc.pdf());
    links << LinkEntry{0, -1, {20, 20, 120, 35}, "英語ページへ", LinkTarget::Page, 1, {}};
    window.doc.commit(replaceLinks(window.doc.pdf(), links));
    window.refresh(true);
    window.show();
    auto before = window.doc.pdf();
    writeCandidate(before, output + "/links-scan-before.pdf");
    QString error;
    QTimer messages;
    QObject::connect(&messages, &QTimer::timeout,
                     [&]
                     {
                         if (auto box =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (box->windowTitle() != "OCR結果")
                                 error = box->text();
                             box->accept();
                         }
                     });
    messages.start(10);
    window.language->setCurrentIndex(0);
    window.scope->setCurrentIndex(0);
    window.startOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 90000),
          "Bilingual OCR after link creation");
    check(error.isEmpty(), error);
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Fixed JP/EN text found after link OCR");
    check(editableLinks(window.doc.pdf()).size() == 1 &&
              signatures(window.doc.pdf(), 0).size() == 1,
          "Links and signatures survive OCR");
    for (int page = 0; page < 2; ++page)
        check(renderPage(before, page, .8) == renderPage(window.doc.pdf(), page, .8),
              "OCR visible pixels unchanged");
    auto ocr = window.doc.pdf();
    window.undoAction->trigger();
    check(window.doc.pdf() == before, "OCR Undo keeps links and signatures");
    window.redoAction->trigger();
    check(window.doc.pdf() == ocr, "OCR Redo restores searchable text");
    window.doc.save(output + "/links-scan-ocr.pdf");
    auto reopened = readPdf(window.doc.target);
    auto edited = editableLinks(reopened);
    edited[0].description = "OCR保存後のリンク変更";
    auto updated = replaceLinks(reopened, edited);
    check(pageText(updated, 0).contains("市民公園"), "Reediting retains OCR");
    writeCandidate(updated, output + "/links-scan-reedited.pdf");
    return {{"real_bilingual_OCR", true},
            {"visible_difference", 0},
            {"OCR_Undo_Redo_keeps_links", true}};
}
} // namespace tatsu
