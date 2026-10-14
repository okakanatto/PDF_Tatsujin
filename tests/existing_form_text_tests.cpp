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
QJsonObject plan(const QString& fixtures, const QString& name = "criteria.json")
{
    QFile f(fixtures + "/existing-text-edit/forms/" + name);
    check(f.open(QIODevice::ReadOnly), "Read frozen grouped text conditions");
    return QJsonDocument::fromJson(f.readAll()).object();
}
ExistingTextBlock selected(const PDFDocument& pdf, const QString& value)
{
    for (const auto& block : existingTextBlocks(pdf, 0, {}, true))
        if (block.depth == 2 && block.text == value)
            return block;
    fail("Expected first grouped text missing");
}
void retained(const PDFDocument& source, const PDFDocument& candidate, int selected)
{
    const auto before = existingTextBlocks(source, 0, {}, true);
    const auto after = existingTextBlocks(candidate, 0, {}, true);
    for (const auto& block : before)
        if (block.occurrence != selected)
        {
            auto found = std::find_if(after.begin(), after.end(), [&](const auto& b)
                                      { return b.occurrence == block.occurrence; });
            check(found != after.end() && found->text == block.text && found->font == block.font &&
                      found->physical == block.physical,
                  "Other grouped calls and direct text unchanged");
        }
}
} // namespace
QJsonObject testExistingFormText(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    const auto path = fixtures + "/existing-text-edit/forms/shared-forms.pdf";
    check(fileHash(path).toHex() ==
              fixed["files"].toObject()["shared-forms.pdf"].toString().toLatin1(),
          "Frozen shared text source SHA");
    const auto source = readPdf(path);
    const auto initial = encodePdf(source);
    for (const auto& block : existingTextBlocks(source, 0))
        check(block.depth == 0, "Default text mode excludes groups");
    const auto block = selected(source, fixed["selected"].toString());
    check(block.restriction.isEmpty() && block.font == "Helvetica",
          "Shared font alias resolved within owning group");
    auto english = editExistingText(source, 0, block.occurrence, ExistingTextChange::Replace,
                                    fixed["english"].toString(), {}, {}, true);
    retained(source, english, block.occurrence);
    writeCandidate(english, output + "/existing-form-text-english.pdf");
    auto japanese =
        replaceExistingTextFont(source, 0, block.occurrence, fixed["japanese"].toString(),
                                fixed["explicit_family"].toString(), {}, true);
    retained(source, japanese, block.occurrence);
    Document doc;
    doc.open(path);
    doc.commit(japanese);
    doc.undo();
    check(encodePdf(doc.pdf()) == initial, "Grouped text Undo exact original");
    doc.redo();
    check(encodePdf(doc.pdf()) == encodePdf(japanese), "Grouped text Redo exact candidate");
    doc.save(output + "/existing-form-text-japanese.pdf");
    Document reopened;
    reopened.open(output + "/existing-form-text-japanese.pdf");
    const auto saved = selected(reopened.pdf(), fixed["japanese"].toString());
    reopened.commit(replaceExistingTextFont(reopened.pdf(), 0, saved.occurrence,
                                            fixed["reedited"].toString(),
                                            fixed["explicit_family"].toString(), {}, true));
    retained(source, reopened.pdf(), block.occurrence);
    reopened.save(output + "/existing-form-text-reedited.pdf");
    auto lines = replaceExistingTextLines(
        source, 0, block.occurrence, fixed["three_lines"].toString(),
        fixed["leading_ratio"].toDouble(), fixed["explicit_family"].toString(), {}, true);
    retained(source, lines, block.occurrence);
    writeCandidate(lines, output + "/existing-form-text-lines.pdf");
    const auto geometry = plan(fixtures, "geometry-criteria.json");
    const auto r = geometry["geometry"].toArray();
    auto moved = editExistingText(
        source, 0, block.occurrence, ExistingTextChange::Geometry, {},
        {r[0].toDouble(), r[1].toDouble(), r[2].toDouble(), r[3].toDouble()}, {}, true);
    retained(source, moved, block.occurrence);
    writeCandidate(moved, output + "/existing-form-text-moved.pdf");
    auto removed =
        editExistingText(source, 0, block.occurrence, ExistingTextChange::Remove, {}, {}, {}, true);
    retained(source, removed, block.occurrence);
    writeCandidate(removed, output + "/existing-form-text-removed.pdf");
    int refused = 0;
    for (const auto& name : fixed["refused"].toArray())
    {
        const auto input = fixtures + "/existing-text-edit/forms/" + name.toString();
        check(fileHash(input).toHex() ==
                  fixed["files"].toObject()[name.toString()].toString().toLatin1(),
              "Frozen grouped refusal SHA");
        try
        {
            existingTextBlocks(readPdf(input), 0, {}, true);
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    }
    const auto outside = geometry["outside"].toArray();
    try
    {
        editExistingText(source, 0, block.occurrence, ExistingTextChange::Geometry, {},
                         {outside[0].toDouble(), outside[1].toDouble(), outside[2].toDouble(),
                          outside[3].toDouble()},
                         {}, true);
    }
    catch (const std::exception& e)
    {
        check(QString::fromUtf8(e.what()).contains("表示範囲"), "Group BBox reason");
        ++refused;
    }
    try
    {
        replaceExistingTextFont(
            source, 0, block.occurrence, fixed["japanese"].toString(),
            fixed["explicit_family"].toString(), [] { return true; }, true);
    }
    catch (const std::exception&)
    {
        ++refused;
    }
    check(refused == 5 && encodePdf(source) == initial, "Grouped refusals and cancel atomic");
    return {{"shared_call_isolation", true},
            {"Undo_Redo_save_reopen", true},
            {"geometry_remove_lines", true},
            {"refusals", refused}};
}
QJsonObject testExistingFormTextUi(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    const auto source = readPdf(fixtures + "/existing-text-edit/forms/shared-forms.pdf");
    const auto initial = encodePdf(source);
    ExistingTextDialog dialog(source, 0);
    dialog.show();
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    auto ready = [&]
    {
        check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
              "Grouped text UI preview finished");
    };
    ready();
    auto include = dialog.findChild<QCheckBox*>("existingTextIncludeGroups");
    check(!include->isChecked(), "Grouped text UI opt-in");
    QTest::mouseClick(include, Qt::LeftButton);
    ready();
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    int row = -1;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text().startsWith(fixed["selected"].toString()))
        {
            row = i;
            break;
        }
    check(row >= 0 && list->item(row)->text().contains("2階層"),
          "Grouped text depth and selected call visible");
    list->setCurrentRow(row);
    ready();
    auto input = dialog.findChild<QPlainTextEdit*>("existingTextInput");
    input->setFocus();
    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(fixed["japanese"].toString());
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    auto font = dialog.findChild<QComboBox*>("existingTextFontFamily");
    font->setCurrentIndex(font->findData(fixed["explicit_family"].toString()));
    ready();
    auto apply = dialog.findChild<QPushButton*>("applyExistingText");
    check(apply->isEnabled(), "Grouped Japanese candidate can apply");
    auto operation = dialog.findChild<QComboBox*>("existingTextOperation");
    auto x = dialog.findChild<QDoubleSpinBox*>("existingTextX");
    auto y = dialog.findChild<QDoubleSpinBox*>("existingTextY");
    auto width = dialog.findChild<QDoubleSpinBox*>("existingTextWidth");
    auto height = dialog.findChild<QDoubleSpinBox*>("existingTextHeight");
    auto message = dialog.findChild<QLabel*>("existingTextMessage");
    const auto geometry = plan(fixtures, "geometry-criteria.json");
    const auto outside = geometry["outside"].toArray();
    constexpr double mm = 72.0 / 25.4;
    operation->setCurrentIndex(1);
    x->setValue(outside[0].toDouble() / mm);
    y->setValue(outside[1].toDouble() / mm);
    width->setValue(outside[2].toDouble() / mm);
    height->setValue(outside[3].toDouble() / mm);
    check(QTest::qWaitFor([&] { return message->text().contains("表示範囲"); }, 15000),
          "Grouped UI explains refused BBox overflow");
    check(!apply->isEnabled() && encodePdf(source) == initial,
          "Grouped UI overflow cannot partially apply");
    operation->setCurrentIndex(0);
    ready();
    check(apply->isEnabled(), "Grouped UI recovers to valid Japanese candidate");
    check(dialog.grab().save(output + "/existing-form-text-ui.png"),
          "Actual grouped text UI screenshot");
    check(encodePdf(source) == initial, "Grouped preview has not committed");
    apply->click();
    const auto candidate = dialog.takeDocument();
    retained(source, candidate, selected(source, fixed["selected"].toString()).occurrence);
    writeCandidate(candidate, output + "/existing-form-text-ui.pdf");
    return {{"actual_group_toggle_selection_Japanese_apply", true}, {"native_IME", "未実行"}};
}
} // namespace tatsu
