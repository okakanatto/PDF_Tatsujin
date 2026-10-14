#include "form_data_tests.h"
#include "encrypted_pdf.h"
#include "form_data_dialog.h"
#include "page_operations.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& text)
{
    if (!value)
        fail(text);
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
    fail("Invalid form data operation succeeded");
}
void writeData(const QString& path, const QByteArray& data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Write synthetic packet");
    check(file.write(data) == data.size(), "Packet bytes complete");
}
QVector<FormDataValue> packet()
{
    return {{"name", {"髙橋 香織"}}, {"notes", {"東京都\n申請内容 & <追記>"}},
            {"agree", {"Off"}},      {"choice", {"B"}},
            {"combo", {"Red"}},      {"list", {"West"}}};
}
FormField named(const PDFDocument& document, const QString& name)
{
    for (const auto& field : formFields(document))
        if (field.qualifiedName == name)
            return field;
    fail("Missing test field");
}
} // namespace
QJsonObject testFormDataLifecycle(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D07.pdf");
    const auto hash = fileHash(document.source);
    document.putSignature(0, "入力データと署名を保持", {60, 50}, 12, Qt::black);
    const auto before = document.pdf();
    const auto original = encodePdf(before);
    const int cursor = document.cursor;
    const auto data = packet();
    check(decodeXfdf(encodeXfdf(data)) == data, "All six kind packet roundtrip");
    writeData(output + "/form-data-input.xfdf", encodeXfdf(data));
    auto result = importFormData(before, data);
    check(result.changes.size() == 6, "Six real field changes");
    check(encodePdf(document.pdf()) == original && document.cursor == cursor,
          "Private import leaves original and Undo unchanged");
    writeCandidate(before, output + "/form-data-before.pdf");
    document.commit(result.document);
    document.save(output + "/form-data-after.pdf");
    document.undo();
    check(document.pdf() == before, "One Undo restores every imported field");
    document.redo();
    exportFormData(document.pdf(), output + "/form-data-export.xfdf");
    const auto cursorAfter = document.cursor, saved = document.saved;
    const auto target = document.target;
    QFile exported(output + "/form-data-export.xfdf");
    check(exported.open(QIODevice::ReadOnly), "Export exists");
    check(decodeXfdf(exported.readAll()) == formDataValues(document.pdf()).fields,
          "Export contains current field values");
    check(document.cursor == cursorAfter && document.saved == saved && document.target == target &&
              fileHash(document.source) == hash,
          "Export preserves PDF and source state");
    Document reopened;
    reopened.open(document.target);
    check(named(reopened.pdf(), "name").values == QStringList{"髙橋 香織"} &&
              signatures(reopened.pdf(), 0).size() == 1,
          "Save retains Japanese field and reeditable signature");
    putFormValue(reopened, named(reopened.pdf(), "name").widget, {"再編集した氏名"});
    reopened.save(output + "/form-data-reedited.pdf");
    // Display captions are intentionally identical; identity uses full names.
    PDFDocumentBuilder builder(&before);
    for (const auto& name : {QString("name"), QString("notes")})
    {
        const auto field = named(before, name);
        auto object = *builder.getObjectByReference(field.field).getDictionary();
        const QString caption("同じ表示名");
        const auto bytes = caption.utf16();
        QByteArray text("\xfe\xff", 2);
        for (int i = 0; i < caption.size(); ++i)
        {
            text.append(char(bytes[i] >> 8));
            text.append(char(bytes[i] & 255));
        }
        detail::set(object, "TU", PDFObject::createString(text));
        builder.setObject(field.field, detail::dictObject(object));
    }
    auto aliases = builder.build();
    auto changed =
        importFormData(aliases, {{"name", {"識別した氏名"}}, {"notes", {"別の項目"}}}).document;
    check(named(changed, "name").values == QStringList{"識別した氏名"} &&
              named(changed, "notes").values == QStringList{"別の項目"},
          "Duplicate GUI labels never redirect data");
    writeCandidate(changed, output + "/form-data-aliases.pdf");
    const auto nested =
        QByteArray("<xfdf xmlns='http://ns.adobe.com/xfdf/'><fields><field name='parent'><field "
                   "name='child'><value> preserved </value></field></field></fields></xfdf>");
    check(decodeXfdf(nested) == QVector<FormDataValue>{{"parent.child", {" preserved "}}},
          "Nested full names and whitespace preserve");
    const QVector<FormDataValue> special = {{"空白 & <名>", {" 前後 \r\n改行\r終端\t"}}};
    check(decodeXfdf(encodeXfdf(special)) == special,
          "CR LF tabs and XML special characters exact");
    writeData(output + "/form-data-special.xfdf", encodeXfdf(special));
    // Synthetic multi-select variant, leaving the frozen fixture unchanged.
    PDFDocumentBuilder multiple(&before);
    const auto list = named(before, "list");
    auto dictionary = *multiple.getObjectByReference(list.field).getDictionary();
    detail::set(dictionary, "Ff", PDFObject::createInteger(1 << 21));
    multiple.setObject(list.field, detail::dictObject(dictionary));
    auto multi = importFormData(multiple.build(), {{"list", {"North", "West"}}}).document;
    check(named(multi, "list").values == QStringList{"North", "West"},
          "Multiple selection data kept");
    writeCandidate(multi, output + "/form-data-multiple.pdf");
    PDFDocumentBuilder hierarchy(&before);
    const auto child = named(before, "name");
    PDFDictionary parent;
    detail::set(parent, "T", PDFObject::createString("person"));
    detail::set(parent, "Kids", detail::arrObject({PDFObject::createReference(child.field)}));
    const auto parentRef = hierarchy.addObject(detail::dictObject(parent));
    auto childObject = *hierarchy.getObjectByReference(child.field).getDictionary();
    detail::set(childObject, "T", PDFObject::createString("child"));
    detail::set(childObject, "Parent", PDFObject::createReference(parentRef));
    hierarchy.setObject(child.field, detail::dictObject(childObject));
    const auto formReference = before.getCatalog()->getFormObject();
    auto form = *before.getObject(formReference).getDictionary();
    std::vector<PDFObject> roots;
    for (const auto& entry : *before.getObject(form.get("Fields")).getArray())
        roots.push_back(entry.isReference() && entry.getReference() == child.field
                            ? PDFObject::createReference(parentRef)
                            : entry);
    detail::set(form, "Fields", detail::arrObject(roots));
    if (formReference.isReference())
        hierarchy.setObject(formReference.getReference(), detail::dictObject(form));
    else
        hierarchy.setCatalogAcroForm(hierarchy.addObject(detail::dictObject(form)));
    auto hierarchical = hierarchy.build();
    const auto hierarchicalData =
        decodeXfdf("<xfdf xmlns='http://ns.adobe.com/xfdf/'><fields><field name='person'><field "
                   "name='child'><value>階層の値</value></field></field></fields></xfdf>");
    auto hierarchicalAfter = importFormData(hierarchical, hierarchicalData).document;
    check(named(hierarchicalAfter, "person.child").values == QStringList{"階層の値"},
          "Hierarchical XML changes the correct real PDF field");
    writeCandidate(hierarchicalAfter, output + "/form-data-hierarchy.pdf");
    return {{"six_field_types", true},
            {"single_Undo_Redo_save_reedit", true},
            {"caption_identity_separate", true},
            {"nested_names_CR_XML_special_exact", true},
            {"source_unchanged", true}};
}
QJsonObject testFormDataFailures(const QString& fixtures, const QString& output)
{
    auto original = readPdf(fixtures + "/D07.pdf");
    const auto bytes = encodePdf(original);
    QJsonArray rows;
    auto invalid = [&](QString name, const std::function<void()>& operation)
    {
        rows << QJsonObject{{"case", name}, {"error", rejects(operation)}};
        check(encodePdf(original) == bytes, "Failed data import is atomic");
    };
    invalid("unknown after valid field",
            [&] { importFormData(original, {{"name", {"有効な値"}}, {"unknown", {"no"}}}); });
    invalid("invalid choice after valid field",
            [&] { importFormData(original, {{"name", {"有効な値"}}, {"combo", {"invalid"}}}); });
    invalid("invalid button multiple values",
            [&] { importFormData(original, {{"agree", {"Off", "Yes"}}}); });
    invalid("duplicate names", [&] { encodeXfdf({{"name", {"one"}}, {"name", {"two"}}}); });
    invalid("XML control character", [&] { encodeXfdf({{"name", {QString(QChar(1))}}}); });
    invalid("unsupported glyph after valid field",
            [&] {
                importFormData(original,
                               {{"notes", {"有効な追記"}}, {"name", {QString("\U0010ffff")}}});
            });
    invalid("signed document",
            [&] { importFormData(readPdf(fixtures + "/D08-signed.pdf"), packet()); });
    invalid("over 4MiB", [&] { decodeXfdf(QByteArray(4 * 1024 * 1024 + 1, 'x')); });
    QVector<FormDataValue> excessive;
    for (int i = 0; i < 501; ++i)
        excessive << FormDataValue{QString::number(i), {"value"}};
    invalid("over 500 fields", [&] { encodeXfdf(excessive); });
    const auto prefix = QByteArray("<xfdf xmlns='http://ns.adobe.com/xfdf/'>");
    for (const auto& value :
         {QByteArray("<!DOCTYPE xfdf [<!ENTITY bad SYSTEM 'https://example.invalid/secret'>]>") +
              prefix + "<fields><field name='name'><value>&bad;</value></field></fields></xfdf>",
          prefix + "<annots/></xfdf>",
          prefix + "<fields><field name='name'><value><b>rich</b></value></field></fields></xfdf>",
          prefix + "<fields><field name='name'><value>x</value></field><field "
                   "name='name'><value>y</value></field></fields></xfdf>",
          QByteArray("<xfdf><fields/></xfdf>")})
        invalid("invalid or unsupported XML", [&] { decodeXfdf(value); });
    QByteArray deep = prefix + "<fields>";
    for (int i = 0; i < 17; ++i)
        deep += "<field name='a'>";
    deep += "<value>x</value>";
    for (int i = 0; i < 17; ++i)
        deep += "</field>";
    deep += "</fields></xfdf>";
    invalid("over depth 16", [&] { decodeXfdf(deep); });
    bool cancelled = false;
    invalid("cancel after actual field change",
            [&]
            {
                importFormData(
                    original, packet(), [&] { return cancelled; },
                    [&](int done, int)
                    {
                        if (done == 1)
                            cancelled = true;
                    });
            });
    check(cancelled, "Cancellation after actual field processing");
    PDFDocumentBuilder readOnly(&original);
    const auto field = named(original, "name");
    auto dictionary = *readOnly.getObjectByReference(field.field).getDictionary();
    detail::set(dictionary, "Ff", PDFObject::createInteger(1));
    readOnly.setObject(field.field, detail::dictObject(dictionary));
    invalid("readonly field", [&] { importFormData(readOnly.build(), {{"name", {"changed"}}}); });
    const auto existing = output + "/form-data-existing.xfdf";
    writeData(existing, "KEEP-EXISTING-XFDF");
    const auto hash = fileHash(existing);
    invalid("existing export", [&] { exportFormData(original, existing); });
    check(fileHash(existing) == hash, "Existing data never overwritten");
    int stages = 0;
    const auto cancelledExport = output + "/form-data-cancelled.xfdf";
    invalid("cancel after staged export write",
            [&] { exportFormData(original, cancelledExport, [&] { return ++stages >= 2; }); });
    check(stages >= 2 && !QFileInfo::exists(cancelledExport),
          "Real written data candidate discarded on cancel");
    stages = 0;
    const auto collision = output + "/form-data-collision.xfdf";
    invalid("target appeared before atomic export",
            [&]
            {
                exportFormData(original, collision,
                               [&]
                               {
                                   if (++stages == 3)
                                       writeData(collision, "KEEP-COMPETING-XFDF");
                                   return false;
                               });
            });
    QFile competing(collision);
    check(competing.open(QIODevice::ReadOnly) && competing.readAll() == "KEEP-COMPETING-XFDF",
          "Atomic export cannot overwrite competing data");
    invalid("invalid export path",
            [&] { exportFormData(original, output + "/missing/data.xfdf"); });
    EncryptionOptions options{"form-user-2026", "form-owner-2026"};
    options.copy = false;
    exportProtectedPdf(original, options, output + "/form-data-copy-denied.pdf");
    auto encrypted = readPdf(output + "/form-data-copy-denied.pdf", options.userPassword);
    invalid("copy denied data export", [&] { formDataValues(encrypted); });
    invalid("encrypted import", [&] { importFormData(encrypted, packet()); });
    options.copy = true;
    exportProtectedPdf(original, options, output + "/form-data-copy-permitted.pdf");
    auto permitted = readPdf(output + "/form-data-copy-permitted.pdf", options.userPassword);
    exportFormData(permitted, output + "/form-data-copy-permitted.xfdf");
    check(!editingRestriction(permitted).isEmpty(),
          "Explicit input-value export keeps encrypted PDF read-only");
    if (!qEnvironmentVariable("TATSU_DENIED_SAVE_DIR").isEmpty())
        invalid("NTFS export denial",
                [&] {
                    exportFormData(original,
                                   qEnvironmentVariable("TATSU_DENIED_SAVE_DIR") + "/data.xfdf");
                });
    const auto inert =
        decodeXfdf(prefix + "<f href='https://example.invalid/never-open'/><fields><field "
                            "name='name'><value>safe</value></field></fields></xfdf>");
    check(inert == QVector<FormDataValue>{{"name", {"safe"}}},
          "Document references are inert input metadata");
    return {{"rejected", rows}, {"no_partial_reflection", true}, {"volume_full", "未実行"}};
}
QJsonObject testFormDataUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.openFile(fixtures + "/D07.pdf");
    window.doc.putSignature(0, "データ操作前の署名", {60, 50}, 12, Qt::black);
    window.refresh(true);
    window.show();
    const auto before = window.doc.pdf();
    const auto cursor = window.doc.cursor;
    int ticks = 0, mode = 0;
    QString error;
    const auto input = output + "/form-data-ui-input.xfdf";
    writeData(input, encodeXfdf(packet()));
    auto operate = [&](bool importing)
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
                auto dialog = dynamic_cast<FormDataDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                try
                {
                    check(deadline.elapsed() < 25000, "Form data UI deadline");
                    auto path = dialog->findChild<QLineEdit*>("formDataPath");
                    auto apply = dialog->findChild<QPushButton*>("formDataApply");
                    auto load = dialog->findChild<QPushButton*>("formDataLoad");
                    if (state == 0)
                    {
                        dialog->resize(640, 480);
                        path->setText(importing ? input : output + "/form-data-existing.xfdf");
                        if (importing)
                            QTest::mouseClick(load, Qt::LeftButton);
                        else
                            QTest::mouseClick(apply, Qt::LeftButton);
                        state = 1;
                        if (mode == 1)
                        {
                            bool running = false;
                            for (auto thread : dialog->findChildren<QThread*>())
                                running |= thread->isRunning();
                            check(running, "Owned data worker running before cancel");
                            dialog->reject();
                            state = 5;
                        }
                    }
                    else if (state == 1 && importing && apply->isEnabled())
                    {
                        check(window.doc.pdf() == before && window.doc.cursor == cursor,
                              "Preview has not committed to original");
                        check(dialog->findChild<QTableWidget*>("formDataValues")->rowCount() == 6,
                              "Actual change preview contains six fields");
                        check(!apply->visibleRegion().isEmpty(), "Compact apply visible");
                        dialog->grab().save(output + "/form-data-import-ui.png");
                        if (mode == 2)
                        {
                            writeData(input, encodeXfdf({{"name", {"externally changed"}}}));
                            QTest::mouseClick(apply, Qt::LeftButton);
                            check(!apply->isEnabled(), "Changed input invalidates apply");
                            dialog->reject();
                            state = 5;
                        }
                        else
                        {
                            QTest::mouseClick(apply, Qt::LeftButton);
                            state = 5;
                        }
                    }
                    else if (state == 1 && !importing && apply->isEnabled())
                    {
                        check(dialog->findChild<QLabel*>("formDataMessage")
                                  ->text()
                                  .contains("保存先"),
                              "Export conflict supports retry");
                        path->setText(output + "/form-data-ui-export.xfdf");
                        dialog->grab().save(output + "/form-data-export-ui.png");
                        QTest::mouseClick(apply, Qt::LeftButton);
                        state = 5;
                    }
                }
                catch (const std::exception& exception)
                {
                    error = QString::fromUtf8(exception.what());
                    dialog->reject();
                    state = 5;
                }
            });
        timer.start(3);
        window.manageFormData(importing);
        timer.stop();
        check(error.isEmpty(), error);
    };
    operate(true);
    check(window.doc.cursor == cursor + 1, "Actual explicit import makes one commit");
    const auto imported = window.doc.pdf();
    window.undoAction->trigger();
    check(window.doc.pdf() == before, "Product Undo restores all values");
    mode = 1;
    operate(true);
    check(window.doc.pdf() == before, "Worker cancel changes nothing");
    mode = 2;
    operate(true);
    check(window.doc.pdf() == before, "Changed packet failure changes nothing");
    window.redoAction->trigger();
    check(window.doc.pdf() == imported, "Product Redo restores complete import");
    mode = 0;
    operate(false);
    check(QFileInfo::exists(output + "/form-data-ui-export.xfdf") && window.doc.pdf() == imported,
          "Actual export and retry leave PDF intact");
    window.doc.save(output + "/form-data-ui.pdf");
    window.openFile(fixtures + "/D01.pdf");
    check(!window.findChild<QAction*>("importFormData")->isEnabled() &&
              !window.findChild<QAction*>("exportFormData")->isEnabled(),
          "Data actions disabled when there are no fields");
    return {{"actual_preview_apply_Undo_Redo_cancel_changed_input_export_retry", true},
            {"GUI_event_ticks", ticks},
            {"native_UI", "未実行"}};
}
QJsonObject testFormDataOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.history = {mergeDocuments(
        {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6}), readPdf(fixtures + "/D07.pdf")})};
    window.doc.saved = -1;
    window.doc.putSignature(0, "OCRとフォーム値を保持", {30, 30}, 10, Qt::black);
    window.refresh(true);
    window.show();
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
    window.scope->setCurrentIndex(2);
    window.range->setText("1,2");
    window.startOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 90000),
          "Actual bilingual OCR alongside form page");
    check(error.isEmpty(), error);
    auto before = window.doc.pdf();
    writeCandidate(before, output + "/form-data-ocr-before.pdf");
    auto candidate =
        importFormData(before, {{"name", {"髙橋 香織"}}, {"notes", {"OCRと入力データの併用"}}});
    window.doc.commit(candidate.document);
    for (int page = 0; page < 2; ++page)
        check(pageText(before, page) == pageText(window.doc.pdf(), page) &&
                  renderPage(before, page, .8) == renderPage(window.doc.pdf(), page, .8),
              "OCR scan text and pixels unchanged");
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Fixed bilingual search terms retained");
    const auto after = window.doc.pdf();
    window.doc.undo();
    check(window.doc.pdf() == before, "Import Undo retains OCR");
    window.doc.redo();
    check(window.doc.pdf() == after, "Import Redo restores form values");
    window.doc.save(output + "/form-data-ocr-after.pdf");
    exportFormData(window.doc.pdf(), output + "/form-data-ocr.xfdf");
    auto reopened = readPdf(window.doc.target);
    check(signatures(reopened, 0).size() == 1 &&
              named(reopened, "name").values == QStringList{"髙橋 香織"},
          "Save retains signature, OCR and form data");
    return {{"actual_bilingual_OCR_form_import_Undo_Redo_save_export", true},
            {"scan_pixel_difference", 0},
            {"OCR_text_exact", true}};
}
} // namespace tatsu
