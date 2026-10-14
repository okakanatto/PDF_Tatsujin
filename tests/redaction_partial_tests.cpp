#include "redaction_partial_tests.h"
#include "pdf_objects.h"
#include "redaction_dialog.h"
#include "redaction_document.h"
#include "redaction_ui_test_support.h"
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
QJsonArray cases(const QString& fixtures)
{
    const auto path = fixtures + "/redaction-copy/partial-text/criteria.json";
    check(fileHash(path).toHex() ==
              "fdde07694bcdc03c679b56d8aaacd81150809cdea3e6862150b0a8d5411dbffa",
          "Frozen additional partial redaction criteria");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read partial redaction criteria");
    auto rows = QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray();
    const auto compoundPath = fixtures + "/redaction-copy/partial-composite-v3/criteria.json";
    check(fileHash(compoundPath).toHex() ==
              "894aa5fc4fab49b3af99676c08f34b225924b0344d402d0f27fcdbde8f919ea7",
          "Frozen surviving composite glyph criteria");
    QFile compound(compoundPath);
    check(compound.open(QIODevice::ReadOnly), "Read composite criteria");
    for (const auto& value :
         QJsonDocument::fromJson(compound.readAll()).object()["cases"].toArray())
    {
        auto row = value.toObject();
        row["folder"] = "partial-composite-v3";
        row["output_file"] = "partial-composite-" + row["file"].toString();
        rows.append(row);
    }
    return rows;
}
QRectF rectangle(const QJsonObject& row)
{
    const auto values = row["rect"].toArray();
    return {values[0].toDouble(), values[1].toDouble(), values[2].toDouble(), values[3].toDouble()};
}
PDFDocument input(const QString& fixtures, const QJsonObject& row)
{
    const auto path = fixtures + "/redaction-copy/" + row["folder"].toString("partial-text") + "/" +
                      row["file"].toString();
    check(fileHash(path).toHex() == row["sha256"].toString().toLatin1(), "Frozen partial source");
    return readPdf(path);
}
void retained(PDFDocument document, const QJsonObject& row, PDFDocument baseline)
{
    const auto text = pageText(document, 0);
    check(!text.contains(row["remove"].toString()), "Removed target text");
    for (const auto& value : row["keep"].toArray())
    {
        const auto originalText = pageText(baseline, 0);
        if (row["file"] == "TJ-spacing.pdf" && value == "KEEP_FOLLOWING_LINE" &&
            !originalText.contains(value.toString()))
            check(text.section('\n', -1) == originalText.section('\n', -1),
                  "Keep exact input Qt extraction of spaced following line; independent engines "
                  "check frozen word");
        else
            check(text.contains(value.toString()), "Retained text: " + value.toString());
    }
}
} // namespace
QJsonObject testRedactionPartial(const QString& fixtures, const QString& output)
{
    QJsonArray outputs;
    for (const auto& value : cases(fixtures))
    {
        const auto row = value.toObject();
        auto source = input(fixtures, row);
        const auto before = encodePdf(source);
        const auto name = row["file"].toString();
        const auto destination = output + "/" + row["output_file"].toString("partial-" + name);
        exportRedactedPdf(source, {{0, {rectangle(row)}}}, destination);
        retained(readPdf(destination), row, source);
        check(encodePdf(source) == before, "Partial redaction source snapshot unchanged");
        outputs << QJsonObject{{"file", name}, {"sha256", QString(fileHash(destination).toHex())}};
    }
    return {{"cases", outputs}, {"source_unchanged", true}};
}
QJsonObject testRedactionPartialFailures(const QString& fixtures, const QString& output)
{
    const auto row = cases(fixtures)[1].toObject();
    const auto source = input(fixtures, row);
    const auto before = encodePdf(source);
    int rejected = 0;
    auto reject = [&](const PDFDocument& document, QRectF bounds, QString name,
                      std::function<bool()> cancel = {})
    {
        const auto path = output + "/partial-rejected-" + name + ".pdf";
        bool failed = false;
        try
        {
            exportRedactedPdf(document, {{0, {bounds}}}, path, cancel);
        }
        catch (const std::exception&)
        {
            failed = true;
        }
        check(failed && !QFileInfo::exists(path) && encodePdf(source) == before,
              "Unsafe partial candidate refused atomically: " + name);
        ++rejected;
    };
    const auto bounds = rectangle(row);
    reject(source, {bounds.x() + 2, bounds.y() + 2, 1, 1}, "cut-glyph");
    int observations = 0;
    prepareRedactionCandidate(source, {{0, {bounds}}},
                              [&]
                              {
                                  ++observations;
                                  return false;
                              });
    for (const int at : {1, observations / 2, observations - 1})
    {
        int calls = 0;
        reject(source, bounds, "cancel-" + QString::number(at), [&] { return ++calls >= at; });
        check(calls == at, "Cancellation reaches actual font/resource processing");
    }
    const auto fonts = source.getObject(source.getCatalog()->getPage(0)->getResources())
                           .getDictionary()
                           ->get("Font");
    for (const auto key : {"ToUnicode", "FontFile2"})
    {
        PDFObject original;
        const auto dictionary = source.getObject(fonts).getDictionary();
        for (size_t i = 0; i < dictionary->getCount(); ++i)
        {
            const auto font = source.getObject(dictionary->getValue(i)).getDictionary();
            if (font->hasKey("ToUnicode"))
                original =
                    QByteArray(key) == "ToUnicode"
                        ? font->get(key)
                        : source.getObject(font->get("FontDescriptor")).getDictionary()->get(key);
        }
        check(original.isReference(), "Original resource for alias test");
        for (const bool duplicate : {false, true})
        {
            pdf::PDFDocumentBuilder builder(&source);
            auto catalog =
                *source.getObject(source.getTrailerDictionary()->get("Root")).getDictionary();
            auto alias = original;
            if (duplicate)
                alias = PDFObject::createReference(builder.addObject(source.getObject(original)));
            detail::set(catalog, "TatsujinPartialPrivateAlias", alias);
            builder.setObject(source.getTrailerDictionary()->get("Root").getReference(),
                              detail::dictObject(catalog));
            reject(builder.build(), bounds,
                   QString::fromLatin1(key) + (duplicate ? "-copy" : "-ref"));
        }
    }
    return {{"rejected", rejected}, {"checkpoints", observations}, {"source_unchanged", true}};
}
QJsonObject testRedactionPartialUi(const QString& fixtures, const QString& output)
{
    const auto rows = cases(fixtures);
    QJsonArray checked;
    for (const int index : {1, 7, 10})
    {
        const auto row = rows[index].toObject();
        Window original;
        const auto path = fixtures + "/redaction-copy/partial-text/" + row["file"].toString();
        original.openFile(path);
        original.doc.putSignature(0, "範囲外の署名", {60, 80}, 12, Qt::black);
        original.refresh();
        const auto before = encodePdf(original.doc.pdf());
        const auto cursor = original.doc.cursor, saved = original.doc.saved;
        const auto hash = fileHash(path);
        const auto destination = output + "/partial-ui-" + row["file"].toString();
        RedactionDialog dialog(original.doc.pdf(), 0, {{path, hash}}, destination);
        dialog.show();
        testing::ready(dialog, 0);
        testing::add(
            dialog,
            pageMatrix(original.doc.pdf().getCatalog()->getPage(0)).mapRect(rectangle(row)));
        dialog.findChild<QCheckBox*>("redactionConsent")->setChecked(true);
        check(dialog.grab().save(output + "/partial-ui-" + QString::number(index) + ".png"),
              "Range UI screenshot");
        dialog.findChild<QPushButton*>("saveRedactedCopy")->click();
        check(QTest::qWaitFor(
                  [&]
                  { return !dialog.findChild<QProgressBar*>("redactionProgress")->isVisible(); },
                  30000) &&
                  !dialog.savedPath().isEmpty(),
              dialog.findChild<QLabel*>("redactionMessage")->text());
        Window copy;
        copy.openFile(destination);
        retained(copy.doc.pdf(), row, input(fixtures, row));
        check(signatures(copy.doc.pdf(), 0).size() == 1 && copy.doc.cursor == 0 &&
                  copy.doc.saved == 0,
              "New copy retains signature and contains no original history");
        copy.doc.putSignature(0, "再編集した署名", {60, 80}, 12, Qt::black);
        copy.doc.save(output + "/partial-reedited-" + row["file"].toString());
        check(encodePdf(original.doc.pdf()) == before && original.doc.cursor == cursor &&
                  original.doc.saved == saved && fileHash(path) == hash,
              "Source, dirty state and Undo unchanged");
        original.doc.undo();
        original.doc.redo();
        check(encodePdf(original.doc.pdf()) == before, "Original Undo/Redo remains usable");
        checked << row["file"];
    }
    return {{"UI_cases", checked}, {"native_GUI_OS_clipboard", "未実行; Qt offscreen"}};
}
} // namespace tatsu
