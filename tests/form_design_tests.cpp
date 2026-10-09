#include "form_design_tests.h"
#include "form_data.h"
#include "form_design.h"
#include "form_design_dialog.h"
#include "form_font.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "window.h"
#include <QInputMethodEvent>
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
QString id()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
FormDesignEntry entry(FormKind kind, int number, QRectF rectangle, int page = 0)
{
    FormDesignEntry value;
    value.id = id();
    value.name = QString("designed-%1").arg(number);
    value.caption = QString("設計項目 %1").arg(number);
    value.kind = kind;
    value.widgets << FormDesignWidget{id(), page, rectangle};
    if (kind == FormKind::Checkbox)
    {
        value.exports = {"同意"};
        value.labels = {"同意します"};
        value.values = {"同意"};
    }
    else if (kind == FormKind::Radio)
    {
        value.exports = {"第一", "第二"};
        value.labels = {"第一の選択肢", "第二の選択肢"};
        value.values = {"第二"};
        value.widgets << FormDesignWidget{id(), page, rectangle.translated(45, 0)};
    }
    else if (kind == FormKind::Combo || kind == FormKind::List)
    {
        value.exports = {"東京", "大阪", "京都"};
        value.labels = {"東京都", "大阪府", "京都府"};
        value.values = {"大阪"};
        if (kind == FormKind::List)
        {
            value.multiple = true;
            value.values = {"東京", "京都"};
        }
    }
    else
        value.values = {kind == FormKind::Text ? "髙橋 𠮷野" : "複数行の入力\n山田 太郎"};
    return value;
}
FormField named(const PDFDocument& document, const QString& name, QString state = {})
{
    for (const auto& field : formFields(document))
        if (field.qualifiedName == name && (state.isEmpty() || field.onState == state))
            return field;
    fail("Designed field was not found: " + name);
}
} // namespace
QJsonObject testFormDesignLifecycle(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D07.pdf");
    const auto originalHash = fileHash(document.source);
    const auto originalBody = pageText(document.pdf(), 0);
    const auto foreign = formFields(document.pdf());
    document.putSignature(0, "フォーム設計前の署名", {300, 370}, 18, Qt::black);
    const auto original = document.pdf();
    const auto cursor = document.cursor;
    QVector<FormDesignEntry> values;
    values << entry(FormKind::Text, 1, {45, 210, 260, 32})
           << entry(FormKind::Multiline, 2, {45, 250, 260, 65})
           << entry(FormKind::Checkbox, 3, {330, 210, 20, 20})
           << entry(FormKind::Radio, 4, {330, 250, 20, 20})
           << entry(FormKind::Combo, 5, {45, 405, 260, 32})
           << entry(FormKind::List, 6, {45, 455, 260, 100});
    values[0].maxLength = 40;
    values[0].required = true;
    const auto candidate = replaceFormDesign(document.pdf(), values);
    check(document.pdf() == original && document.cursor == cursor, "Design candidate is private");
    document.commit(candidate);
    check(document.cursor == cursor + 1 && formDesign(document.pdf()).size() == 6,
          "Six kinds are one document Undo unit");
    check(formFields(document.pdf()).size() == foreign.size() + 7,
          "Foreign field and seven new widgets preserved");
    document.save(output + "/designed-six.pdf");
    auto empty = values;
    for (auto& item : empty)
    {
        item.values.clear();
        if (item.kind == FormKind::Text || item.kind == FormKind::Multiline)
            item.maxLength = 0;
    }
    writeCandidate(replaceFormDesign(original, empty), output + "/designed-external-empty.pdf");
    auto image = renderPage(document.pdf(), 0, 1.4);
    check(image.save(output + "/designed-six.png"), "Design render evidence");
    document.undo();
    check(document.pdf() == original && document.dirty(),
          "Undo restores previous unsaved signature");
    document.redo();
    check(renderPage(document.pdf(), 0, 1.4) == image, "Redo restores exact new form appearances");
    document.open(output + "/designed-six.pdf");
    const auto beforeInput = document.pdf();
    putFormValue(document, named(document.pdf(), "designed-1").widget, {"山田 太郎 髙橋 𠮷野"});
    putFormValue(document, named(document.pdf(), "designed-3").widget, {"Off"});
    putFormValue(document, named(document.pdf(), "designed-4", "第一").widget, {"第一"});
    putFormValue(document, named(document.pdf(), "designed-5").widget, {"京都"});
    putFormValue(document, named(document.pdf(), "designed-6").widget, {"大阪", "京都"});
    check(named(document.pdf(), "designed-1").values == QStringList{"山田 太郎 髙橋 𠮷野"},
          "New Japanese and supplementary values entered without partial loss");
    check(named(document.pdf(), "designed-4", "第二").values == QStringList{"第一"},
          "Radio widgets share their canonical Japanese state value");
    document.save(output + "/designed-filled.pdf");
    auto edited = formDesign(document.pdf());
    for (auto& item : edited)
        if (item.name == "designed-1")
        {
            item.widgets[0].rectangle.translate(18, 12);
            item.widgets[0].rectangle.setWidth(300);
            item.caption = "再編集した氏名欄";
        }
    const auto beforeDesign = document.pdf();
    document.commit(replaceFormDesign(document.pdf(), edited));
    check(named(document.pdf(), "designed-1").values == named(beforeDesign, "designed-1").values,
          "Re-design preserves the current entered value");
    document.undo();
    check(document.pdf() == beforeDesign, "Re-design Undo preserves all filled values");
    document.redo();
    document.save(output + "/designed-reedited.pdf");
    const auto xfdf = encodeXfdf(formDataValues(document.pdf()).fields);
    check(decodeXfdf(xfdf) == formDataValues(document.pdf()).fields,
          "New fields work with the existing XFDF value workflow");
    QFile data(output + "/designed-values.xfdf");
    check(data.open(QIODevice::WriteOnly) && data.write(xfdf) == xfdf.size(),
          "XFDF evidence written");
    check(pageText(document.pdf(), 0) == originalBody &&
              fileHash(document.source) != QByteArray{} &&
              fileHash(fixtures + "/D07.pdf") == originalHash &&
              signatures(document.pdf(), 0).size() == 1,
          "Existing body, signature and source preserved");
    QJsonArray pages;
    Document geometry;
    geometry.open(fixtures + "/D02.pdf");
    QVector<FormDesignEntry> placements;
    for (int page = 0; page < geometry.pages(); ++page)
        placements << entry(FormKind::Text, page + 10, {36, 45, 180, 32}, page);
    geometry.commit(replaceFormDesign(geometry.pdf(), placements));
    geometry.save(output + "/designed-geometry.pdf");
    for (int page = 0; page < geometry.pages(); ++page)
    {
        const auto field = named(geometry.pdf(), QString("designed-%1").arg(page + 10));
        const auto actual =
            pageMatrix(geometry.pdf().getCatalog()->getPage(page)).mapRect(field.rectangle);
        check(
            (actual.topLeft() - placements[page].widgets[0].rectangle.topLeft()).manhattanLength() <
                    .5 &&
                std::abs(actual.width() - 180) < .5 && std::abs(actual.height() - 32) < .5,
            "Rotated CropBox and UserUnit physical placement within existing 0.5pt bound");
        putFormValue(geometry, field.widget, {"回転後に入力 髙橋 𠮷野"});
        check(renderPage(geometry.pdf(), page, 1)
                  .save(output + QString("/designed-geometry-%1.png").arg(page)),
              "Geometry render evidence");
        pages << QJsonObject{
            {"page", page},
            {"rect", QJsonArray{actual.x(), actual.y(), actual.width(), actual.height()}}};
    }
    geometry.save(output + "/designed-geometry-filled.pdf");
    return {{"kinds", 6},
            {"widgets", 7},
            {"shared_radio", true},
            {"Japanese_supplementary_values", true},
            {"reedit_undo_redo", true},
            {"foreign_fields_body_signature_preserved", true},
            {"XFDF_roundtrip", true},
            {"geometry", pages}};
}
QJsonObject testFormDesignFailures(const QString& fixtures, const QString& output)
{
    Q_UNUSED(output);
    Document document;
    document.open(fixtures + "/D07.pdf");
    document.putSignature(0, "取消前の未保存署名", {350, 400}, 18, Qt::black);
    const auto original = document.pdf();
    const auto revision = document.revision;
    const auto hash = fileHash(document.source);
    const QVector<FormDesignEntry> good{entry(FormKind::Text, 1, {45, 210, 260, 32})};
    int checks = 0;
    auto reject =
        [&](const QVector<FormDesignEntry>& values, const std::function<bool()>& cancel = {})
    {
        bool failed = false;
        try
        {
            replaceFormDesign(document.pdf(), values, cancel);
        }
        catch (const std::exception&)
        {
            failed = true;
        }
        check(failed && document.pdf() == original && document.revision == revision &&
                  document.dirty() && fileHash(document.source) == hash,
              "Invalid design leaves source, unsaved document and Undo intact");
        ++checks;
    };
    auto test = [&](const std::function<void(FormDesignEntry&)>& change)
    {
        auto values = good;
        change(values[0]);
        reject(values);
    };
    test([](auto& value) { value.name = "bad.name"; });
    test([](auto& value) { value.name = QString(257, 'x'); });
    test([](auto& value) { value.name = " "; });
    test([](auto& value) { value.values = {QString(QChar(0xd800))}; });
    test([](auto& value) { value.values = {"single\nline"}; });
    test([](auto& value) { value.values = {QString::fromUcs4(U"\U0010ffff")}; });
    test([](auto& value) { value.widgets[0].rectangle.setX(-1); });
    test([](auto& value) { value.widgets[0].page = 100; });
    test([](auto& value)
         { value.widgets[0].rectangle.setWidth(std::numeric_limits<double>::infinity()); });
    test([](auto& value) { value.maxLength = 1; });
    test([](auto& value) { value.multiple = true; });
    test([](auto& value) { value.id = "not-an-id"; });
    auto duplicate = good;
    duplicate << good[0];
    reject(duplicate);
    int calls = 0;
    reject(good, [&] { return ++calls > 7; });
    reject(good, [] { return true; });
    check(replaceFormDesign(document.pdf(), {}) == document.pdf(),
          "Empty unchanged design does not commit");
    auto created = replaceFormDesign(document.pdf(), good);
    check(formDesign(created).size() == 1, "Recovery succeeds after rejected candidates");
    auto collided = good;
    collided[0].name = formFields(original).front().qualifiedName;
    reject(collided);
    auto choices = QVector<FormDesignEntry>{entry(FormKind::Combo, 2, {45, 210, 260, 32})};
    choices[0].exports[1] = choices[0].exports[0];
    reject(choices);
    auto large = QVector<FormDesignEntry>{entry(FormKind::Combo, 3, {45, 210, 260, 32})};
    large[0].exports.clear();
    large[0].labels.clear();
    large[0].values.clear();
    for (int i = 0; i < 260; ++i)
    {
        large[0].exports << QString("value-%1").arg(i);
        large[0].labels << QString(65536, 'x');
    }
    reject(large);
    choices = {entry(FormKind::List, 2, {45, 210, 260, 90})};
    choices[0].labels[2] = QString::fromUcs4(U"\U0010ffff");
    reject(choices);
    QVector<FormDesignEntry> many;
    for (int i = 0; i < 1000; ++i)
    {
        auto item = entry(FormKind::Text, i + 100, {45, 210, 260, 32});
        item.values = {""};
        many << item;
    }
    const auto thousand = replaceFormDesign(original, many);
    check(formDesign(thousand).size() == 1000, "Exact 1000-widget candidate succeeds");
    many << entry(FormKind::Text, 1101, {45, 210, 260, 32});
    reject(many);
    int completedBeforeCancel = 0;
    bool cancelAfterRewrite = false;
    bool lateCancelled = false;
    auto changed = formDesign(thousand);
    for (auto& item : changed)
        item.caption = "途中で取り消す設計";
    try
    {
        replaceFormDesign(
            thousand, changed, [&] { return cancelAfterRewrite; },
            [&](int completed, int total)
            {
                check(total == 1000, "Actual design progress reports the immutable field count");
                completedBeforeCancel = completed;
                cancelAfterRewrite = completed == 1;
            });
    }
    catch (const std::exception&)
    {
        lateCancelled = true;
    }
    check(lateCancelled && completedBeforeCancel == 1 &&
              formDesign(thousand).front().caption != "途中で取り消す設計",
          "Cancellation after validation and actual rewrites preserves original candidate");
    FormDesignDialog cancelDialog(thousand, 0);
    cancelDialog.show();
    cancelDialog.findChild<QLineEdit*>("formDesignCaption")->setText("ワーカーの取消");
    QTest::mouseClick(cancelDialog.findChild<QPushButton*>("formDesignApply"), Qt::LeftButton);
    QTest::mouseClick(cancelDialog.findChild<QPushButton*>("formDesignCancel"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return !cancelDialog.isVisible(); }, 20000) &&
              cancelDialog.result() != QDialog::Accepted,
          "Actual design worker cancellation does not accept a partial document");
    const auto managed = named(created, "designed-1");
    PDFDocumentBuilder malformed(&created);
    auto widget = *malformed.getObjectByReference(managed.widget).getDictionary();
    detail::set(widget, "Kids", detail::arrObject({PDFObject::createReference(managed.widget)}));
    malformed.setObject(managed.widget, detail::dictObject(widget));
    bool cycleRejected = false;
    try
    {
        formDesign(malformed.build());
    }
    catch (const std::exception&)
    {
        cycleRejected = true;
    }
    check(cycleRejected, "Raw cyclic widget graph rejected before entering SDK recursive parser");
    const auto erased = replaceFormDesign(created, {});
    check(formDesign(erased).isEmpty() && formFields(erased).size() == formFields(original).size(),
          "Delete removes only owned field tree and widgets");
    for (auto name : {"D08-encrypted.pdf", "D08-signed.pdf", "D08-xfa.pdf"})
    {
        const auto protectedPdf =
            readPdf(fixtures + '/' + name,
                    QByteArray(name) == "D08-encrypted.pdf" ? "correct-password" : "");
        bool failed = false;
        try
        {
            replaceFormDesign(protectedPdf, good);
        }
        catch (const std::exception&)
        {
            failed = true;
        }
        check(failed, "Protected document refuses new fields");
        ++checks;
    }
    return {{"rejected_cases", checks},
            {"1000_widgets", true},
            {"fields_rewritten_before_cancel", completedBeforeCancel},
            {"worker_cancel", true},
            {"source_and_unsaved_state_preserved", true},
            {"recovery", true},
            {"owned_only_deletion", true}};
}
QJsonObject testFormDesignUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(1024, 720);
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    const auto hash = fileHash(window.doc.source);
    QString error;
    int mode = 0, ticks = 0;
    auto operate = [&]
    {
        int state = 0;
        QElapsedTimer deadline;
        deadline.start();
        QTimer timer;
        QObject::connect(
            &timer, &QTimer::timeout,
            [&]
            {
                ++ticks;
                auto dialog = dynamic_cast<FormDesignDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                if (deadline.elapsed() > 30000)
                {
                    error = "Form design UI deadline";
                    dialog->reject();
                    return;
                }
                auto preview = dynamic_cast<PageRegionPreview*>(
                    dialog->findChild<QWidget*>("formDesignPreview"));
                auto page = dialog->findChild<QComboBox*>("formDesignPage");
                auto click = [&](const char* key)
                { QTest::mouseClick(dialog->findChild<QPushButton*>(key), Qt::LeftButton); };
                auto drag = [&](QPointF from, QPointF to)
                {
                    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier,
                                      preview->physicalToWidget(from).toPoint());
                    QTest::mouseMove(preview, preview->physicalToWidget(to).toPoint(), 10);
                    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier,
                                        preview->physicalToWidget(to).toPoint());
                };
                try
                {
                    if (state == 0 && !preview->image.isNull())
                    {
                        dialog->resize(800, 480);
                        if (mode == 0)
                        {
                            page->setCurrentIndex(3);
                            state = 1;
                        }
                        else if (mode == 1)
                        {
                            dialog->findChild<QLineEdit*>("formDesignCaption")
                                ->setText("再編集した氏名");
                            dialog->findChild<QPlainTextEdit*>("formDesignInitial")
                                ->setPlainText("再入力 髙橋 𠮷野");
                            state = 4;
                        }
                        else if (mode == 2)
                        {
                            dialog->findChild<QLineEdit*>("formDesignName")
                                ->setText("取消する内部名");
                            click("formDesignCancel");
                            state = 5;
                        }
                        else
                        {
                            click("formDesignDelete");
                            state = 4;
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
                        click("formDesignDraw");
                        drag({60, 180}, {260, 212});
                        check(preview->regions.size() == 1,
                              "Real pointer drawing creates a field candidate");
                        auto box = preview->regions[0].second;
                        drag(box.center(), box.center() + QPointF(20, 15));
                        check(preview->regions[0].second.x() > box.x() + 12,
                              "Actual pointer drag moves field");
                        box = preview->regions[0].second;
                        drag(box.bottomRight(), box.bottomRight() + QPointF(35, 15));
                        check(preview->regions[0].second.width() > box.width() + 20,
                              "Actual pointer corner resizes field");
                        dialog->findChild<QLineEdit*>("formDesignName")->setText("氏名欄");
                        auto initial = dialog->findChild<QPlainTextEdit*>("formDesignInitial");
                        initial->setPlainText("単行に\n改行");
                        click("formDesignApply");
                        state = 3;
                    }
                    else if (state == 3 &&
                             dialog->findChild<QPushButton*>("formDesignApply")->isEnabled() &&
                             dialog->findChild<QLabel*>("formDesignMessage")
                                 ->text()
                                 .contains("制御文字"))
                    {
                        auto initial = dialog->findChild<QPlainTextEdit*>("formDesignInitial");
                        initial->clear();
                        QInputMethodEvent commit;
                        commit.setCommitString("山田 太郎 髙橋 𠮷野");
                        QCoreApplication::sendEvent(initial, &commit);
                        check(initial->toPlainText() == "山田 太郎 髙橋 𠮷野",
                              "Synthetic IME commit survives property synchronization");
                        initial->moveCursor(QTextCursor::End);
                        QTest::keyClicks(initial, " A");
                        check(initial->toPlainText().endsWith(" A"),
                              "Typing does not reset the caret while updating preview");
                        state = 4;
                    }
                    else if (state == 4 &&
                             dialog->findChild<QPushButton*>("formDesignApply")->isEnabled())
                    {
                        check(dialog->grab().save(output +
                                                  QString("/form-design-ui-%1.png").arg(mode)),
                              "Compact form designer screenshot");
                        for (const auto key :
                             {"formDesignDraw", "formDesignApply", "formDesignCancel"})
                        {
                            auto control = dialog->findChild<QPushButton*>(key);
                            check(control->visibleRegion().contains(control->rect()),
                                  "Primary controls fit compact dialog");
                        }
                        if (mode != 3)
                        {
                            auto scroll = dialog->findChild<QScrollArea*>("formDesignProperties");
                            for (const auto key :
                                 {"formDesignName", "formDesignInitial", "formDesignHeight"})
                            {
                                auto control = dialog->findChild<QWidget*>(key);
                                scroll->ensureWidgetVisible(control);
                                QTest::qWait(15);
                                check(control->visibleRegion().contains(control->rect()),
                                      "Scrolled settings remain reachable in compact dialog");
                            }
                        }
                        click("formDesignApply");
                        state = 5;
                    }
                }
                catch (const std::exception& failure)
                {
                    error = QString::fromUtf8(failure.what());
                    dialog->reject();
                }
            });
        timer.start(15);
        const auto before = window.doc.pdf();
        const int cursor = window.doc.cursor;
        window.designForms();
        timer.stop();
        check(error.isEmpty(), error);
        check(window.doc.cursor == cursor + (mode == 2 ? 0 : 1),
              "Window commits one Undo unit, cancelling commits none");
        if (mode == 2)
            check(window.doc.pdf() == before, "Cancelled UI retains document snapshot");
    };
    operate();
    check(named(window.doc.pdf(), "氏名欄").values == QStringList{"山田 太郎 髙橋 𠮷野 A"},
          "Actual designer publishes the entered Unicode value");
    window.doc.save(output + "/form-design-ui.pdf");
    window.openFile(window.doc.target);
    mode = 1;
    operate();
    window.doc.save(output + "/form-design-ui-reedited.pdf");
    window.canvas->fitPage();
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "Main viewer form ready");
    const auto field = named(window.doc.pdf(), "氏名欄");
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, field.rectangle.center()).toPoint());
    auto editor = window.canvas->findChild<QLineEdit*>("activeFormEditor");
    check(editor, "New field is editable in the normal viewer");
    editor->setText("閲覧画面で再入力 𠮷野");
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);
    check(QGuiApplication::clipboard()->text() == editor->text(),
          "Offscreen Qt copy preserves Japanese and supplementary field text");
    window.canvas->finishFormEdit();
    check(named(window.doc.pdf(), "氏名欄").values == QStringList{"閲覧画面で再入力 𠮷野"},
          "Normal viewer commits the created form value");
    window.doc.save(output + "/form-design-ui-filled.pdf");
    mode = 2;
    operate();
    const auto beforeDelete = window.doc.pdf();
    mode = 3;
    operate();
    check(formDesign(window.doc.pdf()).isEmpty(), "Actual delete removes the owned field");
    window.undoAction->trigger();
    check(window.doc.pdf() == beforeDelete, "UI Undo restores the deleted field and its value");
    check(fileHash(fixtures + "/D02.pdf") == hash, "Pointer UI workflow preserves original");
    return {{"pointer_draw_move_resize", true},
            {"compact_dialog", true},
            {"invalid_retry", true},
            {"preview_timer_ticks", ticks},
            {"synthetic_IME_commit", true},
            {"Qt_copy", true},
            {"normal_viewer_input", true},
            {"reedit_delete_undo", true},
            {"native_IME", "未実行"},
            {"OS_display_scaling", "未実行"}};
}
QJsonObject testFormDesignOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(1024, 720);
    window.show();
    window.openFile(fixtures + "/D03.pdf");
    const auto sourceHash = fileHash(window.doc.source);
    window.doc.putSignature(2, "OCR前の署名", {380, 80}, 16, Qt::black);
    QVector<FormDesignEntry> entries;
    entries << entry(FormKind::Text, 20, {45, 18, 260, 32}, 2)
            << entry(FormKind::Combo, 21, {45, 18, 260, 32}, 6);
    window.doc.commit(replaceFormDesign(window.doc.pdf(), entries));
    window.refresh();
    const auto beforeOcr = window.doc.pdf();
    const auto beforeImage = renderPage(window.doc.pdf(), 2, 1);
    writeCandidate(beforeOcr, output + "/designed-ocr-before.pdf");
    QStringList dialogs;
    QString dialogError;
    QTimer messages;
    QObject::connect(&messages, &QTimer::timeout,
                     [&]
                     {
                         if (auto box =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             dialogs << box->windowTitle();
                             if (box->windowTitle() != "OCR結果")
                                 dialogError = box->text();
                             box->accept();
                         }
                     });
    messages.start(10);
    window.scope->setCurrentIndex(2);
    window.range->setText("3,7");
    window.startOcr();
    check(window.worker && window.doc.busy,
          "Actual application OCR worker started for Japanese and English scan pages");
    check(QTest::qWaitFor([&] { return !window.worker; }, 120000),
          "Actual form-plus-OCR worker completed");
    check(dialogError.isEmpty(), dialogError);
    check(dialogs == QStringList{"OCR結果"}, "Only the expected successful OCR result dialog");
    messages.stop();
    check(!window.doc.busy && pageText(window.doc.pdf(), 2).contains("市民公園") &&
              pageText(window.doc.pdf(), 6).contains("coastal", Qt::CaseInsensitive),
          "Frozen Japanese and English search words found after OCR");
    check(formDesign(window.doc.pdf()) == formDesign(beforeOcr) &&
              signatures(window.doc.pdf(), 2).size() == 1,
          "Complete new form definitions, values, appearance and prior signature survive OCR");
    check(renderPage(window.doc.pdf(), 2, 1) == beforeImage,
          "OCR changes no visible page or form content");
    window.doc.undo();
    check(window.doc.pdf() == beforeOcr, "OCR Undo retains new form design and prior signature");
    window.doc.redo();
    window.doc.save(output + "/designed-ocr.pdf");
    window.canvas->goToPage(2);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(2); }, 15000),
          "OCR viewer page ready");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    window.query->setText("市民公園");
    QTest::keyClick(window.query, Qt::Key_Return);
    const auto search = window.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              10000),
          "Actual viewer searches saved OCR with designed fields");
    window.canvas->setZoom(.5);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(2); }, 15000),
          "Form-plus-OCR selection page ready");
    window.canvas->setFocus();
    const auto size = pageSize(window.doc.pdf().getCatalog()->getPage(2));
    const auto first = window.canvas->mapFromScene({1, 1}),
               last = window.canvas->mapFromScene({size.width() - 1, size.height() - 1});
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 60);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "Actual viewer OCR text selection with designed fields");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto copied = QApplication::clipboard()->text();
    check(copied.size() > 500 && copied.contains("市民公園"),
          "Actual Qt copy from OCR with new fields");
    auto field = named(window.doc.pdf(), "designed-20");
    putFormValue(window.doc, field.widget, {"OCR後の再入力 𠮷野"});
    auto edited = formDesign(window.doc.pdf());
    for (auto& item : edited)
        if (item.name == "designed-20")
            item.widgets[0].rectangle.translate(12, 8);
    window.doc.commit(replaceFormDesign(window.doc.pdf(), edited));
    window.doc.save(output + "/designed-ocr-reedited.pdf");
    check(pageText(window.doc.pdf(), 2).contains("市民公園") &&
              named(window.doc.pdf(), "designed-20").values == QStringList{"OCR後の再入力 𠮷野"} &&
              fileHash(fixtures + "/D03.pdf") == sourceHash,
          "Saved OCR remains searchable after form input and re-design; original preserved");
    return {{"actual_OCR_worker", true},
            {"Japanese_English_pages", QJsonArray{3, 7}},
            {"visible_difference_pixels", 0},
            {"forms_signature_preserved", true},
            {"viewer_search", true},
            {"reedit_after_OCR", true},
            {"copied_characters", copied.size()},
            {"Qt_copy", true},
            {"OS_clipboard", "未実行"},
            {"original_unchanged", true}};
}
} // namespace tatsu
