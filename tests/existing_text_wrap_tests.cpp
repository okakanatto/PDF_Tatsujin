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
QJsonObject plan(const QString& fixtures)
{
    QFile file(fixtures + "/existing-text-edit/wrapped/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen wrapping criteria");
    return QJsonDocument::fromJson(file.readAll()).object();
}
ExistingTextBlock selected(const PDFDocument& doc, const QString& text, bool forms = false)
{
    for (const auto& block : existingTextBlocks(doc, 0, {}, forms))
        if (block.text == text)
            return block;
    fail("Expected wrapping text missing");
}
void retained(const PDFDocument& original, const PDFDocument& candidate, int occurrence,
              bool forms = false)
{
    const auto before = existingTextBlocks(original, 0, {}, forms);
    const auto after = existingTextBlocks(candidate, 0, {}, forms);
    check(before.size() == after.size(), "Wrapping text block count unchanged");
    for (int i = 0; i < before.size(); ++i)
        if (before[i].occurrence != occurrence)
            check(before[i].text == after[i].text && before[i].font == after[i].font &&
                      before[i].physical == after[i].physical,
                  "Wrapping retains other text and positions");
}
} // namespace
QJsonObject testExistingTextWrap(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    const auto path = fixtures + "/existing-text-edit/wrapped/uniform.pdf";
    check(fileHash(path).toHex() == fixed["files"].toObject()["uniform.pdf"].toString().toLatin1(),
          "Frozen wrapping source SHA");
    const auto source = readPdf(path);
    const auto initial = encodePdf(source);
    const auto block = selected(source, fixed["selected"].toString());
    for (const auto& name : {QString("english"), QString("long_word"), QString("japanese")})
    {
        const auto family = name == "japanese" ? fixed["family"].toString() : QString();
        const auto candidate = replaceExistingTextWrapped(
            source, 0, block.occurrence, fixed[name].toString(),
            fixed[name + "_width_pt"].toDouble(), fixed["leading_ratio"].toDouble(), family);
        writeCandidate(candidate, output + "/existing-text-wrapped-observed-" + name + ".pdf");
        const auto actual = existingTextBlocks(candidate, 0);
        const auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& b)
                                        { return b.occurrence == block.occurrence; });
        check(found != actual.end(), "Observed wrapping block present");
        check(found->text == fixed[name + "_wrapped"].toString(),
              name + ": observed " + found->text + "; expected " +
                  fixed[name + "_wrapped"].toString());
        const auto wrapped = selected(candidate, fixed[name + "_wrapped"].toString());
        check(wrapped.physical.width() <= fixed[name + "_width_pt"].toDouble() + .5,
              "Actual wrapped glyph width respects frozen bound");
        retained(source, candidate, block.occurrence);
        writeCandidate(candidate, output + "/existing-text-wrapped-" + name + ".pdf");
    }
    const auto japanese = readPdf(output + "/existing-text-wrapped-japanese.pdf");
    Document document;
    document.open(path);
    document.commit(japanese);
    document.undo();
    check(encodePdf(document.pdf()) == initial, "Wrap Undo exact original");
    document.redo();
    check(encodePdf(document.pdf()) == encodePdf(japanese), "Wrap Redo exact candidate");
    const auto saved = selected(japanese, fixed["japanese_wrapped"].toString());
    document.commit(replaceExistingTextWrapped(
        document.pdf(), 0, saved.occurrence, fixed["reedited"].toString(),
        fixed["japanese_width_pt"].toDouble(), fixed["leading_ratio"].toDouble(),
        fixed["family"].toString()));
    selected(document.pdf(), fixed["reedited_wrapped"].toString());
    document.save(output + "/existing-text-wrapped-reedited.pdf");
    const auto narrow =
        replaceExistingTextWrapped(source, 0, block.occurrence, fixed["japanese"].toString(),
                                   fixed["narrow_width_pt"].toDouble(),
                                   fixed["leading_ratio"].toDouble(), fixed["family"].toString());
    selected(narrow, fixed["narrow_wrapped"].toString());
    retained(source, narrow, block.occurrence);
    writeCandidate(narrow, output + "/existing-text-wrapped-narrow.pdf");
    const auto grouped = readPdf(fixtures + "/existing-text-edit/forms/shared-forms.pdf");
    const auto group = selected(grouped, "Form first line\nForm second line", true);
    const auto form = replaceExistingTextWrapped(
        grouped, 0, group.occurrence, fixed["japanese"].toString(),
        fixed["japanese_width_pt"].toDouble(), fixed["leading_ratio"].toDouble(),
        fixed["family"].toString(), {}, true);
    selected(form, fixed["japanese_wrapped"].toString(), true);
    retained(grouped, form, group.occurrence, true);
    writeCandidate(form, output + "/existing-text-wrapped-form.pdf");
    int refused = 0;
    auto refuse = [&](auto action)
    {
        try
        {
            action();
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    };
    refuse(
        [&] {
            replaceExistingTextWrapped(source, 0, block.occurrence, fixed["english"].toString(), 1,
                                       1.25);
        });
    refuse([&] { replaceExistingTextWrapped(source, 0, block.occurrence, "leading ", 200, 1.25); });
    refuse(
        [&]
        {
            replaceExistingTextWrapped(source, 0, block.occurrence, QStringList(65, "A").join('\n'),
                                       200, 1.25);
        });
    refuse([&] { replaceExistingTextWrapped(source, 0, block.occurrence, "日本語", 200, 1.25); });
    const auto rotated = readPdf(fixtures + "/existing-text-edit/wrapped/rotated.pdf");
    const auto rotatedBlock = selected(rotated, fixed["selected"].toString());
    refuse(
        [&]
        {
            replaceExistingTextWrapped(rotated, 0, rotatedBlock.occurrence,
                                       fixed["english"].toString(), 200, 1.25);
        });
    refuse(
        [&]
        {
            replaceExistingTextWrapped(source, 0, block.occurrence, fixed["english"].toString(),
                                       200, 1.25, {}, [] { return true; });
        });
    check(refused == 6 && encodePdf(source) == initial, "Wrap refusal/cancel atomic");
    QFile overflowFile(fixtures + "/existing-text-edit/wrapped/overflow-criteria.json");
    check(overflowFile.open(QIODevice::ReadOnly), "Read pre-fixed wrap overflow cases");
    const auto overflow = QJsonDocument::fromJson(overflowFile.readAll()).object();
    auto reasonRefusal = [&](auto action, const QString& reason)
    {
        try
        {
            action();
        }
        catch (const std::exception& e)
        {
            check(QString::fromUtf8(e.what()).contains(reason), "Wrap overflow reason");
            ++refused;
        }
    };
    reasonRefusal(
        [&]
        {
            replaceExistingTextWrapped(
                source, 0, block.occurrence,
                QStringList(overflow["page_lines"].toInt(), overflow["page_text"].toString())
                    .join('\n'),
                overflow["page_width_pt"].toDouble(), fixed["leading_ratio"].toDouble());
        },
        overflow["expected_page_reason"].toString());
    const auto groupedInitial = encodePdf(grouped);
    reasonRefusal(
        [&]
        {
            replaceExistingTextWrapped(grouped, 0, group.occurrence, fixed["japanese"].toString(),
                                       overflow["group_width_pt"].toDouble(),
                                       fixed["leading_ratio"].toDouble(),
                                       fixed["family"].toString(), {}, true);
        },
        overflow["expected_group_reason"].toString());
    check(refused == 8 && encodePdf(source) == initial && encodePdf(grouped) == groupedInitial,
          "Wrap page/group overflow is atomic");
    return {{"word_Japanese_long_word_width", true},
            {"save_reedit_history_shared_call", true},
            {"refusals", refused}};
}
QJsonObject testExistingTextWrapUi(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    const auto source = readPdf(fixtures + "/existing-text-edit/wrapped/uniform.pdf");
    const auto initial = encodePdf(source);
    ExistingTextDialog dialog(source, 0);
    dialog.show();
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    auto ready = [&]
    {
        check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
              "Wrap UI preview ready");
    };
    ready();
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    int row = -1;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == fixed["selected"].toString())
        {
            row = i;
            break;
        }
    check(row >= 0, "Wrap UI source selected");
    list->setCurrentRow(row);
    ready();
    auto operation = dialog.findChild<QComboBox*>("existingTextOperation");
    operation->setCurrentIndex(4);
    auto input = dialog.findChild<QPlainTextEdit*>("existingTextInput");
    input->setFocus();
    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(fixed["japanese"].toString());
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    auto font = dialog.findChild<QComboBox*>("existingTextFontFamily");
    font->setCurrentIndex(font->findData(fixed["family"].toString()));
    auto width = dialog.findChild<QDoubleSpinBox*>("existingTextWrapWidth");
    auto leading = dialog.findChild<QDoubleSpinBox*>("existingTextLeadingRatio");
    leading->setValue(fixed["leading_ratio"].toDouble());
    width->setValue(1 / (72.0 / 25.4));
    auto message = dialog.findChild<QLabel*>("existingTextMessage");
    auto apply = dialog.findChild<QPushButton*>("applyExistingText");
    check(QTest::qWaitFor([&] { return message->text().contains("幅"); }, 15000),
          "Wrap UI narrow rejection reason");
    check(!apply->isEnabled() && encodePdf(source) == initial, "Wrap UI invalid is noncommitting");
    width->setValue(fixed["japanese_width_pt"].toDouble() / (72.0 / 25.4));
    ready();
    check(apply->isEnabled(), "Wrap UI valid recovery");
    check(dialog.grab().save(output + "/existing-text-wrapped-ui.png"),
          "Actual wrap UI screenshot");
    apply->click();
    const auto candidate = dialog.takeDocument();
    selected(candidate, fixed["japanese_wrapped"].toString());
    retained(source, candidate, selected(source, fixed["selected"].toString()).occurrence);
    writeCandidate(candidate, output + "/existing-text-wrapped-ui.pdf");
    return {{"actual_UI_width_font_Japanese_rejection_recovery_apply", true},
            {"native_IME", "未実行"}};
}
} // namespace tatsu
