#include "page_decoration_tests.h"
#include "annotation_operations.h"
#include "form_fields.h"
#include "page_decoration_dialog.h"
#include "page_operations.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "search_panel.h"
#include "text_font.h"
#include "window.h"
#include <QtTest/QTest>
#include <limits>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
DecorationOptions headers()
{
    DecorationOptions options;
    options.header = {"日本語 Header", "中央", "資料"};
    options.footer = {"作成済", "{page} / {pages}", "END"};
    options.size = 9;
    options.color = QColor("#0055bb");
    options.margins = {4, 6, 8, 10};
    options.startNumber = 7;
    return options;
}
DecorationOptions watermark()
{
    DecorationOptions options;
    options.kind = DecorationKind::Watermark;
    options.watermark = "社外秘 SAMPLE";
    options.size = 22;
    options.color = QColor("#bb0000");
    options.opacity = .25;
    options.angle = -30;
    return options;
}
PDFObject pageEntry(const PDFDocument& document, int page, const char* key)
{
    const auto object =
        document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
    return document.getObject(object.getDictionary()->get(key));
}
void retained(const PDFDocument& before, const PDFDocument& after)
{
    check(before.getCatalog()->getPageCount() == after.getCatalog()->getPageCount(),
          "Page count retained");
    for (int i = 0; i < int(before.getCatalog()->getPageCount()); ++i)
    {
        const auto old = before.getCatalog()->getPage(i), page = after.getCatalog()->getPage(i);
        check(old->getCropBox() == page->getCropBox() &&
                  old->getMediaBox() == page->getMediaBox() &&
                  old->getPageRotation() == page->getPageRotation() &&
                  old->getUserUnit() == page->getUserUnit(),
              "Page geometry retained");
        for (const auto& key : {"Contents", "Resources"})
            check(pageEntry(before, i, key) == pageEntry(after, i, key),
                  "Page contents/resources retained");
        const auto annotations = pageEntry(before, i, "Annots");
        if (annotations.isArray())
            for (const auto& value : *annotations.getArray())
                check(before.getObject(value) == after.getObject(value),
                      "Existing annotation or widget byte-object retained");
    }
}
template <typename F> QString rejected(F operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
    fail("Invalid page decoration succeeded");
}
void ready(PageDecorationDialog& dialog)
{
    check(
        QTest::qWaitFor(
            [&] { return dialog.findChild<QPushButton*>("decorationApply")->isEnabled(); }, 20000),
        "Preview ready: " + dialog.findChild<QLabel*>("decorationMessage")->text());
}
} // namespace
QJsonObject testDecorationLifecycle(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    const auto originalHash = fileHash(document.source);
    for (int page = 0; page < 4; ++page)
        document.putSignature(page, "維持する署名",
                              document.pdf().getCatalog()->getPage(page)->getCropBox().center(), 10,
                              Qt::black);
    putAnnotation(document, 0, OverlayKind::Rectangle, {120, 120, 20, 20}, "維持する注釈",
                  Qt::green, 1);
    const auto before = document.pdf();
    const int cursor = document.cursor;
    writeCandidate(before, output + "/decoration-before.pdf");
    auto first = putDecoration(before, {0, 1, 2, 3}, headers());
    retained(before, first);
    document.commit(first);
    check(document.cursor == cursor + 1, "Headers use one Undo unit for all pages");
    document.undo();
    check(document.pdf() == before, "One Undo restores full source");
    document.redo();
    check(document.pdf() == first, "Redo restores all headers");
    writeCandidate(first, output + "/decoration-headers.pdf");
    auto second = putDecoration(first, {0, 1, 2, 3}, watermark());
    retained(before, second);
    document.commit(second);
    const auto groups = decorationGroups(second);
    check(groups.size() == 2 && groups[0].pages == QVector<int>({0, 1, 2, 3}),
          "Independent groups on all rotated pages");
    document.undo();
    check(document.pdf() == first, "Watermark Undo keeps headers");
    document.redo();
    document.save(output + "/decoration-both.pdf");
    for (int page = 0; page < 4; ++page)
    {
        check(renderPage(document.pdf(), page, 1.5)
                  .save(output + QString("/decoration-qt-%1.png").arg(page + 1)),
              "Actual rotated decorations render");
        check(renderPage(document.pdf(), page, 1.5) ==
                  renderPage(document.pdf(), page, 1.5, true, true, RenderPurpose::Print),
              "Decoration print flag includes same appearance");
    }
    Document reopened;
    reopened.open(document.target);
    auto savedGroups = decorationGroups(reopened.pdf());
    check(savedGroups.size() == 2 && savedGroups[0].options.header[0] == "日本語 Header" &&
              savedGroups[0].options.startNumber == 7 && savedGroups[1].options.opacity == .25,
          "Settings reopen exactly");
    auto changed = savedGroups[0].options;
    changed.header[0] = "再編集済み";
    reopened.commit(putDecoration(reopened.pdf(), {1, 3}, changed, savedGroups[0].id));
    auto changedGroups = decorationGroups(reopened.pdf());
    auto found = std::find_if(changedGroups.begin(), changedGroups.end(),
                              [&](const auto& item) { return item.id == savedGroups[0].id; });
    check(found != changedGroups.end() && found->pages == QVector<int>({1, 3}) &&
              changedGroups.size() == 2,
          "Group change replaces only requested group and pages");
    reopened.save(output + "/decoration-reedited.pdf");
    const auto edited = reopened.pdf();
    reopened.commit(removeDecoration(reopened.pdf(), savedGroups[0].id));
    check(decorationGroups(reopened.pdf()).size() == 1, "Header removal preserves watermark");
    reopened.undo();
    check(reopened.pdf() == edited, "One Undo restores removed group");
    auto removed = removeDecoration(removeDecoration(second, groups[0].id), groups[1].id);
    for (int i = 0; i < 4; ++i)
        check(pageEntry(before, i, "Annots") == pageEntry(removed, i, "Annots"),
              "Removing owned decorations restores original annotation list");
    const auto reordered = selectPages(second, {3, 1, 0, 2});
    auto contents = [](const PDFDocument& document, int page)
    {
        QVector<PDFObject> result;
        const auto list = pageEntry(document, page, "Annots");
        for (const auto& item : *list.getArray())
        {
            const auto annotation = document.getObject(item);
            if (annotation.getDictionary()->hasKey("TatsujinDecoration"))
            {
                result << annotation.getDictionary()->get("Contents");
                result << annotation.getDictionary()->get("TatsujinDecoration");
            }
        }
        return result;
    };
    check(contents(second, 3) == contents(reordered, 0),
          "Number appearance and settings follow page, not silently renumbered");
    writeCandidate(reordered, output + "/decoration-reordered.pdf");
    Document form;
    form.open(fixtures + "/D07.pdf");
    auto fields = formFields(form.pdf());
    auto name = std::find_if(fields.begin(), fields.end(),
                             [](const auto& field) { return field.name == "name"; });
    check(name != fields.end(), "Form name field found");
    putFormValue(form, name->widget, {"髙橋 香織"});
    writeCandidate(form.pdf(), output + "/decoration-form-before.pdf");
    const auto oldForm = form.pdf();
    auto formOptions = headers();
    if (textFontFamilies().contains("Meiryo UI"))
        formOptions.fontFamily = "Meiryo UI";
    form.commit(putDecoration(form.pdf(), {0}, formOptions));
    retained(oldForm, form.pdf());
    form.save(output + "/decoration-form.pdf");
    QJsonArray opacities;
    for (double value : {.05, .2, .5, 1.})
    {
        auto options = watermark();
        options.opacity = value;
        auto candidate = putDecoration(before, {0}, options);
        writeCandidate(candidate,
                       output + QString("/decoration-opacity-%1.pdf").arg(qRound(value * 100)));
        check(decorationGroups(candidate).front().options.opacity == value,
              "Opacity setting preserved");
        opacities << value;
    }
    check(fileHash(document.source) == originalHash, "Frozen input remains unchanged");
    return {{"rotations", QJsonArray{0, 90, 180, 270}},
            {"groups", 2},
            {"saved_reedit_remove_Undo", true},
            {"form_signature_contents_retained", true},
            {"page_numbers_fixed", true},
            {"form_decoration_font", formOptions.fontFamily},
            {"additional_opacities", opacities}};
}
QJsonObject testDecorationFailures(const QString& fixtures)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    const auto original = encodePdf(source);
    QJsonArray cases;
    auto invalid = [&](QString name, const std::function<void()>& operation)
    {
        cases.append(QJsonObject{{"case", name}, {"error", rejected(operation)}});
        check(encodePdf(source) == original, "Every failure leaves original document intact");
    };
    for (const auto& pages :
         {QVector<int>{}, QVector<int>{-1}, QVector<int>{4}, QVector<int>{0, 0}})
        invalid("invalid pages", [&] { putDecoration(source, pages, headers()); });
    for (double value : {5., 145., std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()})
        invalid("invalid font size",
                [&]
                {
                    auto option = headers();
                    option.size = value;
                    putDecoration(source, {0}, option);
                });
    for (double value : {-1., 101., std::numeric_limits<double>::quiet_NaN()})
        invalid("invalid margins",
                [&]
                {
                    auto option = headers();
                    option.margins.setLeft(value);
                    putDecoration(source, {0}, option);
                });
    for (const auto& text : {QString("{unknown}"), QString("\n"), QString("\t"), QString(201, 'x'),
                             QString(QChar(0x0378))})
        invalid("invalid text",
                [&]
                {
                    auto option = headers();
                    option.header[0] = text;
                    putDecoration(source, {0}, option);
                });
    invalid("overlapping columns",
            [&]
            {
                auto option = headers();
                option.header[0] = QString(80, 'W');
                putDecoration(source, {0, 3}, option);
            });
    invalid("invalid later page", [&] { putDecoration(source, {0, 8}, headers()); });
    invalid("unknown font",
            [&]
            {
                auto option = headers();
                option.fontFamily = "Unavailable PDF Font";
                putDecoration(source, {0}, option);
            });
    invalid("empty watermark",
            [&]
            {
                auto option = watermark();
                option.watermark = "";
                putDecoration(source, {0}, option);
            });
    invalid("oversized rotated watermark",
            [&]
            {
                auto option = watermark();
                option.size = 144;
                putDecoration(source, {0, 3}, option);
            });
    invalid("invalid opacity",
            [&]
            {
                auto option = watermark();
                option.opacity = .01;
                putDecoration(source, {0}, option);
            });
    invalid("invalid angle",
            [&]
            {
                auto option = watermark();
                option.angle = 91;
                putDecoration(source, {0}, option);
            });
    invalid("invalid number",
            [&]
            {
                auto option = headers();
                option.startNumber = 0;
                putDecoration(source, {0}, option);
            });
    invalid("missing group", [&] { removeDecoration(source, QUuid::createUuid().toString()); });
    invalid("empty document", [&] { putDecoration(PDFDocument{}, {0}, headers()); });
    invalid("protected PDF",
            [&] { putDecoration(readPdf(fixtures + "/D08-signed.pdf"), {0}, headers()); });
    bool stop = false;
    int completed = 0;
    invalid("cancel after real page built",
            [&]
            {
                putDecoration(
                    source, {0, 1, 2, 3}, headers(), {}, [&] { return stop; },
                    [&](int done, int)
                    {
                        completed = done;
                        if (done == 1)
                            stop = true;
                    });
            });
    check(completed == 1, "Cancel occurred after actual AP creation, no result published");
    auto decorated = putDecoration(source, {0}, headers());
    const auto list = pageEntry(decorated, 0, "Annots");
    const auto ref = list.getArray()->getItem(list.getArray()->getCount() - 1).getReference();
    PDFDocumentBuilder builder(&decorated);
    auto annotation = *builder.getObjectByReference(ref).getDictionary();
    detail::set(annotation, "TatsujinDecoration", PDFObject::createString("{\"version\":99}"));
    builder.setObject(ref, detail::dictObject(annotation));
    const auto unknown = builder.build();
    const auto unknownBytes = encodePdf(unknown);
    invalid("unknown stored metadata", [&] { putDecoration(unknown, {0}, headers()); });
    check(encodePdf(unknown) == unknownBytes, "Unknown annotation remains intact");
    return {{"rejected", cases},
            {"partial_cancel_completed_pages", completed},
            {"originals_preserved", true}};
}
QJsonObject testDecorationUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(800, 480);
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "Product PDF visible");
    const auto before = window.doc.pdf();
    int ticks = 0, busyTicks = 0, operations = 0;
    QString error;
    auto operate = [&](QString action, int mode)
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
                auto dialog =
                    dynamic_cast<PageDecorationDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                for (auto thread : dialog->findChildren<QThread*>())
                    if (thread->isRunning())
                        ++busyTicks;
                if (elapsed.elapsed() > 30000)
                {
                    error = "Decoration UI deadline";
                    dialog->reject();
                    return;
                }
                try
                {
                    auto apply = dialog->findChild<QPushButton*>("decorationApply");
                    if (state == 0)
                    {
                        dialog->resize(760, 480);
                        if (mode == 0)
                        {
                            dialog->findChild<QComboBox*>("decorationScope")->setCurrentIndex(2);
                            dialog->findChild<QLineEdit*>("decorationRange")->setText("2,4");
                            dialog->findChild<QLineEdit*>("decorationText0")->setText("操作確認");
                            dialog->findChild<QComboBox*>("decorationPreviewPage")
                                ->setCurrentIndex(1);
                            dialog->findChild<QComboBox*>("decorationPreviewPage")
                                ->setCurrentIndex(3);
                            dialog->findChild<QComboBox*>("decorationPreviewPage")
                                ->setCurrentIndex(1);
                        }
                        else if (mode == 1)
                        {
                            dialog->findChild<QLineEdit*>("decorationWatermark")
                                ->setText("透かしの確認");
                            dialog->findChild<QDoubleSpinBox*>("decorationSize")->setValue(20);
                        }
                        else if (mode == 2)
                        {
                            check(
                                dialog->findChild<QComboBox*>("decorationGroup")->currentIndex() ==
                                    1,
                                "Saved group automatically selected");
                            dialog->findChild<QLineEdit*>("decorationText0")
                                ->setText("保存後の変更");
                        }
                        state = 1;
                    }
                    else if (state == 1 && apply->isEnabled())
                    {
                        check(dialog->rect().contains(apply->mapTo(dialog, apply->rect().center())),
                              "Apply visible in compact dialog");
                        if (mode == 0)
                        {
                            dialog->findChild<QLineEdit*>("decorationText1")->setText("{bad}");
                            check(!apply->isEnabled(), "Stale preview cannot be applied");
                            state = 2;
                        }
                        else
                        {
                            dialog->grab().save(output +
                                                QString("/decoration-ui-%1.png").arg(mode));
                            state = 4;
                            QTest::mouseClick(
                                mode == 3 ? dialog->findChild<QPushButton*>("decorationRemove")
                                          : apply,
                                Qt::LeftButton);
                        }
                    }
                    else if (state == 2 && dialog->findChild<QLabel*>("decorationMessage")
                                               ->text()
                                               .contains("{page}"))
                    {
                        dialog->findChild<QLineEdit*>("decorationText1")->clear();
                        state = 3;
                    }
                    else if (state == 3 && apply->isEnabled())
                    {
                        dialog->grab().save(output + "/decoration-ui-0.png");
                        state = 4;
                        QTest::mouseClick(apply, Qt::LeftButton);
                    }
                }
                catch (const std::exception& failure)
                {
                    error = QString::fromUtf8(failure.what());
                    dialog->reject();
                }
            });
        timer.start(1);
        const int cursor = window.doc.cursor;
        auto button = window.findChild<QAction*>(action);
        check(button && button->isEnabled(), "Product formatting action reachable");
        button->trigger();
        timer.stop();
        check(error.isEmpty(), error);
        check(state == 4 && window.doc.cursor == cursor + 1, "Actual dialog commits once");
        ++operations;
    };
    operate("editHeadersFooters", 0);
    auto group = decorationGroups(window.doc.pdf());
    check(group.size() == 1 && group.front().pages == QVector<int>({1, 3}),
          "Actual specified-page headers");
    operate("editWatermark", 1);
    window.doc.save(output + "/decoration-ui-both.pdf");
    window.doc.open(window.doc.target);
    window.refresh(true);
    operate("editHeadersFooters", 2);
    auto changed = decorationGroups(window.doc.pdf());
    auto header = std::find_if(changed.begin(), changed.end(), [](const auto& item)
                               { return item.options.kind == DecorationKind::HeaderFooter; });
    check(header != changed.end() && header->options.header[0] == "保存後の変更",
          "Product saved group reedited");
    const auto edited = window.doc.pdf();
    operate("editHeadersFooters", 3);
    check(decorationGroups(window.doc.pdf()).size() == 1, "Product deletes only header group");
    window.undoAction->trigger();
    check(window.doc.pdf() == edited, "Product Undo restores removed group");
    check(ticks > 2 && busyTicks > 0 && window.doc.pdf() != before,
          "GUI events continue during actual AP and render workers");
    window.doc.save(output + "/decoration-ui-reedited.pdf");
    return {{"operations", operations},
            {"event_loop_ticks", ticks},
            {"worker_busy_ticks", busyTicks},
            {"specified_pages", QJsonArray{2, 4}},
            {"compact_dialog", QJsonArray{760, 480}},
            {"native_UI", "未実行"}};
}
QJsonObject testDecorationCancel(const QString& fixtures)
{
    auto source = readPdf(fixtures + "/D10-digital-100.pdf");
    const auto original = encodePdf(source);
    PageDecorationDialog dialog(source, 0, DecorationKind::HeaderFooter);
    dialog.show();
    ready(dialog);
    QTest::mouseClick(dialog.findChild<QPushButton*>("decorationApply"), Qt::LeftButton);
    check(QTest::qWaitFor(
              [&] {
                  return dialog.findChild<QLabel*>("decorationMessage")->text().startsWith("作成 ");
              },
              15000),
          "Actual multipage apply progress");
    check(!dialog.findChild<QPushButton*>("decorationApply")->isEnabled(),
          "Apply cannot be repeated while busy");
    QTest::mouseClick(dialog.findChild<QPushButton*>("decorationCancel"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return !dialog.isVisible(); }, 15000),
          "Cancel joins owned worker and closes");
    check(dialog.result() != QDialog::Accepted && encodePdf(source) == original,
          "Cancel publishes no partial PDF");
    rejected([&] { dialog.takeDocument(); });
    return {{"actual_apply_cancel", true}, {"pages", 100}, {"no_partial_result", true}};
}
QJsonObject testDecorationOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.doc.history = {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6})};
    window.doc.saved = -1;
    ++window.doc.revision;
    window.doc.putSignature(0, "署名と装飾を保持", {30, 30}, 10, Qt::black);
    window.doc.commit(putDecoration(window.doc.pdf(), {0, 1}, headers()));
    window.doc.commit(putDecoration(window.doc.pdf(), {0, 1}, watermark()));
    window.refresh(true);
    writeCandidate(window.doc.pdf(), output + "/decoration-scan-before.pdf");
    auto before = window.doc.pdf();
    QString error;
    QTimer messages;
    QObject::connect(&messages, &QTimer::timeout,
                     [&]
                     {
                         if (auto message =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (message->windowTitle() != "OCR結果")
                                 error = message->text();
                             message->accept();
                         }
                     });
    messages.start(20);
    window.ocrAction->trigger();
    window.language->setCurrentIndex(0);
    window.scope->setCurrentIndex(0);
    window.startOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 90000),
          "Real bilingual OCR with headers and watermark");
    check(error.isEmpty(), error);
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Fixed JP and EN search text preserved");
    for (int page = 0; page < 2; ++page)
        check(renderPage(before, page, .8) == renderPage(window.doc.pdf(), page, .8),
              "OCR keeps visible decorations and signature pixel-exact");
    check(decorationGroups(window.doc.pdf()).size() == 2 &&
              signatures(window.doc.pdf(), 0).size() == 1,
          "OCR retains reeditable groups and signature");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    window.query->setText("市民公園");
    QTest::keyClick(window.query, Qt::Key_Return);
    auto search = window.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              15000),
          "Actual search with decorations");
    window.canvas->setZoom(.5);
    window.canvas->goToPage(0);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "OCR page ready for copy");
    const auto physical = pageSize(window.doc.pdf().getCatalog()->getPage(0));
    auto first = window.canvas->mapFromScene({1, 1}),
         last = window.canvas->mapFromScene({physical.width() - 1, physical.height() - 1});
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 80);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "OCR text selection with decorations");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    auto copied = QApplication::clipboard()->text();
    check(copied.size() > 500 && copied.contains("市民公園"),
          "Real Qt text copy, no annotation text mixed into body");
    window.doc.save(output + "/decoration-scan-ocr.pdf");
    auto saved = readPdf(window.doc.target);
    check(decorationGroups(saved).size() == 2 &&
              pageText(saved, 1).contains("coastal", Qt::CaseInsensitive),
          "Saved decoration OCR and settings reopen");
    return {{"derived_D03_pages", QJsonArray{3, 7}},
            {"real_bilingual_OCR", true},
            {"visible_difference", 0},
            {"Qt_copy_characters", copied.size()}};
}
} // namespace tatsu
