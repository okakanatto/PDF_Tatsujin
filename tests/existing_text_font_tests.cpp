#include "existing_text_dialog.h"
#include "existing_text_tests.h"
#include "form_fields.h"
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
ExistingTextBlock selected(PDFDocument document, const QString& value)
{
    for (const auto& block : existingTextBlocks(document, 0))
        if (block.text == value)
            return block;
    fail("Fixed font body missing: " + value);
}
void retained(const PDFDocument& source, const PDFDocument& candidate, int occurrence)
{
    const auto before = existingTextBlocks(source, 0), after = existingTextBlocks(candidate, 0);
    check(before.size() == after.size(), "Font replacement retains other blocks");
    for (int i = 0; i < before.size(); ++i)
        if (before[i].occurrence != occurrence)
            check(before[i].text == after[i].text && before[i].physical == after[i].physical &&
                      before[i].font == after[i].font,
                  "Following and unselected font state, glyphs and positions identical");
    check(formFields(candidate).size() == formFields(source).size(),
          "Font replacement retains forms");
}
QJsonObject plan(const QString& fixtures)
{
    QFile file(fixtures + "/existing-text-edit/font-state/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read fixed font-state criteria");
    return QJsonDocument::fromJson(file.readAll()).object();
}
} // namespace
QJsonObject testExistingTextFonts(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    const auto path = fixtures + "/existing-text-edit/font-state/" + fixed["file"].toString();
    check(fileHash(path).toHex() == fixed["sha256"].toString().toLatin1(),
          "Frozen font-state source SHA");
    const auto original = readPdf(path);
    const auto initial = encodePdf(original);
    const auto block = selected(original, fixed["selected"].toString());
    int number = 0;
    for (const auto& family : fixed["families"].toArray())
    {
        Document document;
        document.open(path);
        document.commit(replaceExistingTextFont(document.pdf(), 0, block.occurrence,
                                                fixed["replacement"].toString(),
                                                family.toString()));
        auto changed = document.pdf();
        const auto wanted = selected(changed, fixed["replacement"].toString());
        check(wanted.font != block.font, "Explicit new body font adopted");
        retained(original, changed, block.occurrence);
        const auto bytes = encodePdf(changed);
        document.undo();
        check(encodePdf(document.pdf()) == initial, "New font Undo exact original");
        document.redo();
        check(encodePdf(document.pdf()) == bytes, "New font Redo exact candidate");
        const auto name = number++ == 0 ? "noto" : "meiryo";
        document.save(output + "/existing-text-font-" + name + ".pdf");
        Document reopened;
        reopened.open(output + "/existing-text-font-" + name + ".pdf");
        const auto saved = selected(reopened.pdf(), fixed["replacement"].toString());
        reopened.commit(editExistingText(reopened.pdf(), 0, saved.occurrence,
                                         ExistingTextChange::Replace,
                                         "テストします。本文の編集を"));
        retained(original, reopened.pdf(), block.occurrence);
        reopened.save(output + "/existing-text-font-reedited-" + name + ".pdf");
    }
    bool unsupported = false;
    const char32_t cp = char32_t(fixed["unsupported_scalar"].toInt());
    try
    {
        replaceExistingTextFont(original, 0, block.occurrence, QString::fromUcs4(&cp, 1),
                                "Noto Sans JP");
    }
    catch (const std::exception& error)
    {
        unsupported = QString::fromUtf8(error.what()).contains("対応していない文字");
    }
    check(unsupported && encodePdf(original) == initial,
          "Unsupported explicit font glyph refuses atomically");
    return {{"explicit_Noto_and_Meiryo_UI", true},
            {"font_state_preserved", true},
            {"Undo_Redo_reopen_reedit", true}};
}
QJsonObject testExistingTextFontUi(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    auto source = readPdf(fixtures + "/existing-text-edit/font-state/" + fixed["file"].toString());
    const auto original = encodePdf(source);
    ExistingTextDialog dialog(source, 0);
    dialog.show();
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    auto ready = [&]
    {
        check(QTest::qWaitFor(
                  [&]
                  { return preview->property("renderReady").toBool() && !preview->image.isNull(); },
                  15000),
              "Font candidate preview ready");
    };
    ready();
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    int index = -1;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == fixed["selected"].toString())
            index = i;
    check(index >= 0, "Font UI source block available");
    list->setCurrentRow(index);
    ready();
    auto input = dialog.findChild<QPlainTextEdit*>("existingTextInput");
    input->setFocus();
    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(fixed["replacement"].toString());
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    ready();
    auto apply = dialog.findChild<QPushButton*>("applyExistingText");
    check(!apply->isEnabled(), "Original font cannot silently adopt new Japanese");
    auto families = dialog.findChild<QComboBox*>("existingTextFontFamily");
    const int wanted = families->findData("Meiryo UI");
    check(wanted >= 0, "Installed Meiryo UI is selectable");
    families->setFocus();
    families->setCurrentIndex(wanted);
    ready();
    check(apply->isEnabled(), "Explicit Meiryo UI candidate can apply");
    auto width = dialog.findChild<QDoubleSpinBox*>("existingTextWidth");
    const auto actual = replaceExistingTextFont(
        source, 0, selected(source, fixed["selected"].toString()).occurrence,
        fixed["replacement"].toString(), "Meiryo UI");
    const auto changedBounds = selected(actual, fixed["replacement"].toString()).physical;
    check(qAbs(width->value() * 72.0 / 25.4 - changedBounds.width()) < .0002,
          "Font replacement dimensions describe actual candidate glyphs");
    auto marked = std::find_if(preview->regions.begin(), preview->regions.end(),
                               [index](const auto& region) { return region.first == index; });
    check(marked != preview->regions.end() && marked->second == changedBounds,
          "Font replacement highlight follows actual candidate glyphs");
    const int oldPixels = preview->image.width();
    preview->setZoom(2.0, preview->rect().center());
    ready();
    check(preview->image.width() > oldPixels,
          "Font preview zoom rerenders actual higher resolution");
    check(dialog.grab().save(output + "/existing-text-font-ui.png"), "Actual font UI screenshot");
    apply->click();
    auto candidate = dialog.takeDocument();
    check(selected(candidate, fixed["replacement"].toString()).font !=
              selected(source, fixed["selected"].toString()).font,
          "Font UI adopted selected typeface");
    retained(source, candidate, selected(source, fixed["selected"].toString()).occurrence);
    writeCandidate(candidate, output + "/existing-text-font-ui.pdf");
    check(encodePdf(source) == original, "Font UI leaves original snapshot intact");
    return {{"actual_Japanese_Qt_paste_and_Meiryo_UI_choice", true},
            {"preview_and_explicit_apply", true}};
}
} // namespace tatsu
