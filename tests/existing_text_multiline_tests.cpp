#include "existing_text_dialog.h"
#include "existing_text_tests.h"
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
QJsonObject criteria(const QString& fixtures)
{
    QFile file(fixtures + "/existing-text-edit/multiline/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen multiline conditions");
    return QJsonDocument::fromJson(file.readAll()).object();
}
ExistingTextBlock selected(const PDFDocument& document, const QString& text)
{
    for (const auto& block : existingTextBlocks(document, 0))
        if (block.text == text)
            return block;
    fail("Multiline block missing: " + text);
}
void retained(const PDFDocument& source, const PDFDocument& candidate, int occurrence)
{
    const auto before = existingTextBlocks(source, 0), after = existingTextBlocks(candidate, 0);
    for (const auto& block : before)
        if (block.occurrence != occurrence)
        {
            const auto actual = selected(candidate, block.text);
            check(actual.physical == block.physical && actual.font == block.font,
                  "Multiline edit retains unselected glyphs and inherited font state");
        }
    check(before.size() == after.size(), "Multiline replacement retains all blocks");
}
} // namespace
QJsonObject testExistingTextMultiline(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    const auto path = fixtures + "/existing-text-edit/multiline/uniform.pdf";
    check(fileHash(path).toHex() == fixed["files"].toObject()["uniform.pdf"].toString().toLatin1(),
          "Frozen multiline source SHA");
    const auto source = readPdf(path);
    const auto initial = encodePdf(source);
    const auto block = selected(source, fixed["selected"].toString());
    check(block.restriction.isEmpty() && block.font == "Helvetica",
          "Uniform two-line editable font");
    auto changed = editExistingText(source, 0, block.occurrence, ExistingTextChange::Replace,
                                    fixed["replacement"].toString());
    retained(source, changed, block.occurrence);
    writeCandidate(changed, output + "/existing-text-multiline-english.pdf");
    auto japanese =
        replaceExistingTextFont(source, 0, block.occurrence, fixed["japanese"].toString(),
                                fixed["explicit_family"].toString());
    retained(source, japanese, block.occurrence);
    Document document;
    document.open(path);
    document.commit(japanese);
    document.undo();
    check(encodePdf(document.pdf()) == initial, "Multiline Undo exact original");
    document.redo();
    check(encodePdf(document.pdf()) == encodePdf(japanese), "Multiline Redo exact candidate");
    document.save(output + "/existing-text-multiline-japanese.pdf");
    Document reopened;
    reopened.open(output + "/existing-text-multiline-japanese.pdf");
    const auto saved = selected(reopened.pdf(), fixed["japanese"].toString());
    reopened.commit(replaceExistingTextFont(reopened.pdf(), 0, saved.occurrence,
                                            fixed["reedited"].toString(), "Meiryo UI"));
    retained(source, reopened.pdf(), block.occurrence);
    reopened.save(output + "/existing-text-multiline-reedited.pdf");
    const auto rect = fixed["geometry"].toArray();
    auto moved = editExistingText(
        source, 0, block.occurrence, ExistingTextChange::Geometry, {},
        {rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(), rect[3].toDouble()});
    retained(source, moved, block.occurrence);
    writeCandidate(moved, output + "/existing-text-multiline-moved.pdf");
    auto removed = editExistingText(source, 0, block.occurrence, ExistingTextChange::Remove);
    check(existingTextBlocks(removed, 0).size() + 1 == existingTextBlocks(source, 0).size(),
          "All multiline glyphs removed together");
    writeCandidate(removed, output + "/existing-text-multiline-removed.pdf");
    int refused = 0;
    for (const auto& value : {QString("One line"), QString("one\ntwo\nthree"), QString("first\n")})
        try
        {
            editExistingText(source, 0, block.occurrence, ExistingTextChange::Replace, value);
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    const auto mixedPath = fixtures + "/existing-text-edit/multiline/mixed-fonts.pdf";
    check(fileHash(mixedPath).toHex() ==
              fixed["files"].toObject()["mixed-fonts.pdf"].toString().toLatin1(),
          "Frozen mixed-font refusal source SHA");
    auto mixed = readPdf(mixedPath);
    const auto mixedBlock = selected(mixed, fixed["selected"].toString());
    check(!mixedBlock.restriction.isEmpty(), "Mixed font size explicitly restricted");
    try
    {
        editExistingText(mixed, 0, mixedBlock.occurrence, ExistingTextChange::Replace,
                         fixed["replacement"].toString());
    }
    catch (const std::exception&)
    {
        ++refused;
    }
    check(refused == 4 && encodePdf(source) == initial,
          "Multiline refusals atomic and original intact");
    QFile rotatedPlan(fixtures + "/existing-text-edit/multiline-rotated/criteria.json");
    check(rotatedPlan.open(QIODevice::ReadOnly), "Read frozen rotated multiline plan");
    const auto rotation = QJsonDocument::fromJson(rotatedPlan.readAll()).object();
    const auto rotatedPath =
        fixtures + "/existing-text-edit/multiline-rotated/" + rotation["file"].toString();
    check(fileHash(rotatedPath).toHex() == rotation["sha256"].toString().toLatin1(),
          "Frozen rotated multiline input");
    const auto rotated = readPdf(rotatedPath);
    const auto target = selected(rotated, rotation["selected"].toString());
    const auto bounds = rotation["geometry"].toArray();
    auto rotationCopy = editExistingText(
        rotated, 0, target.occurrence, ExistingTextChange::Geometry, {},
        {bounds[0].toDouble(), bounds[1].toDouble(), bounds[2].toDouble(), bounds[3].toDouble()});
    retained(rotated, rotationCopy, target.occurrence);
    writeCandidate(rotationCopy, output + "/existing-text-multiline-rotated.pdf");
    return {{"two_lines_English_Japanese_reedit", true},
            {"Undo_Redo_save_reopen", true},
            {"move_resize_remove", true},
            {"refusals", refused}};
}
QJsonObject testExistingTextMultilineUi(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    auto source = readPdf(fixtures + "/existing-text-edit/multiline/uniform.pdf");
    ExistingTextDialog dialog(source, 0);
    dialog.show();
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    auto ready = [&]
    {
        check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
              "Multiline UI candidate finished");
    };
    ready();
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    int index = -1;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == fixed["selected"].toString().left(60))
            index = i;
    check(index >= 0, "Multiline UI selected block");
    list->setCurrentRow(index);
    ready();
    auto input = dialog.findChild<QPlainTextEdit*>("existingTextInput");
    input->setFocus();
    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(fixed["japanese"].toString());
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    auto families = dialog.findChild<QComboBox*>("existingTextFontFamily");
    families->setCurrentIndex(families->findData("Meiryo UI"));
    ready();
    auto apply = dialog.findChild<QPushButton*>("applyExistingText");
    check(apply->isEnabled(), "Japanese multiline UI candidate can apply");
    check(dialog.grab().save(output + "/existing-text-multiline-ui.png"),
          "Actual multiline UI screenshot");
    apply->click();
    const auto candidate = dialog.takeDocument();
    retained(source, candidate, selected(source, fixed["selected"].toString()).occurrence);
    writeCandidate(candidate, output + "/existing-text-multiline-ui.pdf");
    return {{"actual_two_line_Qt_paste", true},
            {"explicit_family_and_apply", true},
            {"native_IME", "未実行"}};
}
} // namespace tatsu
