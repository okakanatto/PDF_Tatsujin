#include "existing_text_tests.h"
#include "existing_image_edit.h"
#include "existing_text_edit.h"
#include "form_fields.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
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
PDFDocument source(const QString& fixtures)
{
    const auto base = fixtures + "/existing-text-edit/visible/";
    QFile file(base + "criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen body criteria");
    const auto plan = QJsonDocument::fromJson(file.readAll()).object();
    check(fileHash(base + plan["file"].toString()).toHex() == plan["sha256"].toString().toLatin1(),
          "Frozen body source hash");
    return readPdf(base + plan["file"].toString());
}
int find(const QVector<ExistingTextBlock>& rows, const QString& text)
{
    for (const auto& row : rows)
        if (row.text == text)
            return row.occurrence;
    fail("Fixed text block missing: " + text);
}
void others(const PDFDocument& original, const PDFDocument& candidate, int selected)
{
    const auto before = existingTextBlocks(original, 0), after = existingTextBlocks(candidate, 0);
    for (const auto& first : before)
    {
        if (first.occurrence == selected)
            continue;
        bool found = false;
        for (const auto& second : after)
            if (first.occurrence == second.occurrence)
            {
                check(first.text == second.text && first.physical == second.physical,
                      "Unselected body text and position identical");
                found = true;
            }
        check(found, "Unselected body block retained");
    }
    auto first = original, second = candidate;
    for (int page = 1; page < int(original.getCatalog()->getPageCount()); ++page)
        check(pageText(first, page) == pageText(second, page), "Other page text retained");
    check(formFields(original).size() == formFields(candidate).size(), "Form fields retained");
    const auto fields = formFields(candidate);
    for (const auto& field : fields)
        check(field.values == QStringList{field.qualifiedName == "keep-field"
                                              ? "KEEP_FORM_9df67"
                                              : "SECRET_FORM_9df67"},
              "Form values retained");
    const auto images = existingImages(candidate, 0), oldImages = existingImages(original, 0);
    check(images.size() == oldImages.size(), "All body images retained");
    for (int i = 0; i < images.size(); ++i)
        check(images[i].matrix == oldImages[i].matrix &&
                  images[i].reference == oldImages[i].reference,
              "Image resources and transforms retained");
}
} // namespace
QJsonObject testExistingTextCore(const QString& fixtures, const QString& output)
{
    const auto original = source(fixtures);
    const auto bytes = encodePdf(original);
    const auto rows = existingTextBlocks(original, 0);
    const int english = find(rows, "Original body line"),
              japanese = find(rows, "日本語の本文を編集します");
    for (const auto& pair : QVector<QPair<int, QString>>{{english, "Edited body line"},
                                                         {japanese, "本文の日本語を編集します"}})
    {
        auto changed =
            editExistingText(original, 0, pair.first, ExistingTextChange::Replace, pair.second);
        const auto edited = existingTextBlocks(changed, 0);
        check(find(edited, pair.second) == pair.first, "Fixed replacement text round trip");
        others(original, changed, pair.first);
        const auto suffix = pair.first == english ? "english" : "japanese";
        const auto path = output + "/existing-text-" + suffix + ".pdf";
        writeCandidate(changed, path);
        changed = readPdf(path);
        check(find(existingTextBlocks(changed, 0), pair.second) == pair.first,
              "Saved text editable");
        auto restored = editExistingText(changed, 0, pair.first, ExistingTextChange::Replace,
                                         pair.first == english ? "Original body line"
                                                               : "日本語の本文を編集します");
        others(original, restored, pair.first);
        writeCandidate(restored, output + "/existing-text-restored-" + suffix + ".pdf");
    }
    auto moved = editExistingText(original, 0, english, ExistingTextChange::Geometry, {},
                                  QRectF(160, 120, 300, 30));
    others(original, moved, english);
    const auto movedRows = existingTextBlocks(moved, 0);
    QRectF box;
    for (const auto& row : movedRows)
        if (row.occurrence == english)
            box = row.physical;
    check(qAbs(box.x() - 160) < 1e-6 && qAbs(box.y() - 120) < 1e-6 &&
              qAbs(box.width() - 300) < 1e-6 && qAbs(box.height() - 30) < 1e-6,
          "Exact body glyph geometry");
    writeCandidate(moved, output + "/existing-text-moved.pdf");
    Document history;
    history.open(fixtures + "/existing-text-edit/visible/body-text.pdf");
    const auto initial = encodePdf(history.pdf());
    history.commit(editExistingText(history.pdf(), 0, english, ExistingTextChange::Replace,
                                    "Edited body line"));
    const auto changed = encodePdf(history.pdf());
    check(history.cursor == 1 && history.dirty(), "Body edit one Undo unit");
    history.undo();
    check(encodePdf(history.pdf()) == initial, "Body Undo restores original");
    history.redo();
    check(encodePdf(history.pdf()) == changed, "Body Redo restores candidate");
    auto removed = editExistingText(original, 0, english, ExistingTextChange::Remove);
    others(original, removed, english);
    writeCandidate(removed, output + "/existing-text-removed.pdf");
    check(encodePdf(original) == bytes, "Original snapshot immutable");
    QFile rotatedPlan(fixtures + "/existing-text-edit/rotated/criteria.json");
    check(rotatedPlan.open(QIODevice::ReadOnly), "Read frozen rotated text criteria");
    const auto fixed = QJsonDocument::fromJson(rotatedPlan.readAll()).object();
    const auto path = fixtures + "/existing-text-edit/rotated/" + fixed["file"].toString();
    check(fileHash(path).toHex() == fixed["sha256"].toString().toLatin1(), "Frozen rotated source");
    const auto rotated = readPdf(path);
    const int rotatedIndex = find(existingTextBlocks(rotated, 0), "Original body line");
    const auto rotatedCopy = editExistingText(
        rotated, 0, rotatedIndex, ExistingTextChange::Geometry, {}, QRectF(100, 200, 250, 40));
    others(rotated, rotatedCopy, rotatedIndex);
    writeCandidate(rotatedCopy, output + "/existing-text-rotated.pdf");
    return {{"outputs", 6}, {"japanese_and_english", true}, {"original_immutable", true}};
}
QJsonObject testExistingTextRefusals(const QString& fixtures, const QString&)
{
    const auto original = source(fixtures);
    const auto bytes = encodePdf(original);
    const auto rows = existingTextBlocks(original, 0);
    const int english = find(rows, "Original body line");
    int count = 0;
    auto reject = [&](const std::function<void()>& operation, const QString& reason)
    {
        bool refused = false;
        try
        {
            operation();
        }
        catch (const std::exception& error)
        {
            refused = QString::fromUtf8(error.what()).contains(reason);
        }
        check(refused, "Specific body refusal: " + reason);
        check(encodePdf(original) == bytes, "No partial body mutation");
        ++count;
    };
    reject([&] { editExistingText(original, 0, -1, ExistingTextChange::Remove); },
           "見つかりません");
    const auto invisible = readPdf(fixtures + "/existing-text-edit/body-text.pdf");
    const auto oldRows = existingTextBlocks(invisible, 0);
    reject(
        [&]
        {
            editExistingText(invisible, 0, find(oldRows, "Original body line"),
                             ExistingTextChange::Replace, "Edited body line");
        },
        "不可視OCR");
    reject([&] { editExistingText(original, -1, english, ExistingTextChange::Remove); }, "ページ");
    const auto signedPdf = readPdf(fixtures + "/D08-signed.pdf");
    reject([&] { editExistingText(signedPdf, 0, 0, ExistingTextChange::Remove); }, "証明書");
    reject([&] { editExistingText(original, 0, english, ExistingTextChange::Replace, ""); }, "1行");
    reject([&]
           { editExistingText(original, 0, english, ExistingTextChange::Replace, "two\nlines"); },
           "1行");
    reject([&]
           { editExistingText(original, 0, english, ExistingTextChange::Replace, "未収録漢字"); },
           "字体");
    reject(
        [&]
        {
            editExistingText(original, 0, english, ExistingTextChange::Geometry, {},
                             QRectF(-1, 10, 100, 20));
        },
        "ページ範囲");
    reject(
        [&]
        {
            editExistingText(original, 0, english, ExistingTextChange::Geometry, {},
                             QRectF(1, 1, std::numeric_limits<double>::quiet_NaN(), 10));
        },
        "ページ範囲");
    reject(
        [&]
        {
            editExistingText(original, 0, english, ExistingTextChange::Remove, {}, {},
                             [] { return true; });
        },
        "中止");
    return {{"specific_refusals", count}, {"source_unchanged", true}};
}
} // namespace tatsu
