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
    QFile file(fixtures + "/existing-text-edit/line-count/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen line-count expectations");
    return QJsonDocument::fromJson(file.readAll()).object();
}
ExistingTextBlock target(const PDFDocument& pdf, const QString& text)
{
    for (const auto& block : existingTextBlocks(pdf, 0))
        if (block.text == text)
            return block;
    fail("Expected line-count block missing");
}
void retained(const PDFDocument& source, const PDFDocument& candidate, int occurrence)
{
    const auto before = existingTextBlocks(source, 0), after = existingTextBlocks(candidate, 0);
    check(before.size() == after.size(), "Line-count edit retains text blocks");
    for (const auto& block : before)
        if (block.occurrence != occurrence)
        {
            const auto actual = target(candidate, block.text);
            check(actual.font == block.font && actual.physical == block.physical,
                  "Line-count edit retains unselected font and glyph positions");
        }
}
QString sourcePath(const QString& fixtures, const QJsonObject& fixed, const QString& name)
{
    const auto path =
        fixtures + "/existing-text-edit/" + fixed["source_folder"].toString() + "/" + name + ".pdf";
    check(fileHash(path).toHex() == fixed["files"].toObject()[name + ".pdf"].toString().toLatin1(),
          "Frozen line-count source SHA");
    return path;
}
} // namespace
QJsonObject testExistingTextLineCount(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    int refused = 0;
    for (const auto& name : {QString("relative-position"), QString("relative-leading")})
    {
        const auto path = sourcePath(fixtures, fixed, name);
        const auto source = readPdf(path);
        const auto initial = encodePdf(source);
        const auto block = target(source, fixed["selected"].toString());
        const auto prefix = output + "/existing-text-line-count-" + name + "-";
        const auto ratio = fixed["leading_ratio"].toDouble();
        auto english = replaceExistingTextLines(source, 0, block.occurrence,
                                                fixed["english"].toString(), ratio);
        retained(source, english, block.occurrence);
        writeCandidate(english, prefix + "english.pdf");
        auto japanese =
            replaceExistingTextLines(source, 0, block.occurrence, fixed["japanese"].toString(),
                                     ratio, fixed["explicit_family"].toString());
        retained(source, japanese, block.occurrence);
        Document doc;
        doc.open(path);
        doc.commit(japanese);
        doc.undo();
        check(encodePdf(doc.pdf()) == initial, "Line-count Undo exact original");
        doc.redo();
        check(encodePdf(doc.pdf()) == encodePdf(japanese), "Line-count Redo exact candidate");
        doc.save(prefix + "japanese.pdf");
        Document reopened;
        reopened.open(prefix + "japanese.pdf");
        const auto saved = target(reopened.pdf(), fixed["japanese"].toString());
        reopened.commit(replaceExistingTextLines(reopened.pdf(), 0, saved.occurrence,
                                                 fixed["reedited"].toString(), ratio,
                                                 fixed["explicit_family"].toString()));
        retained(source, reopened.pdf(), block.occurrence);
        reopened.save(prefix + "reedited.pdf");
        const auto negative = fixed["refused"].toObject();
        QStringList tooMany;
        for (int i = 0; i < negative["line_count"].toInt(); ++i)
            tooMany << "line";
        for (const auto& text : {negative["empty_line"].toString(), tooMany.join('\n')})
            try
            {
                replaceExistingTextLines(source, 0, block.occurrence, text, ratio);
            }
            catch (const std::exception&)
            {
                ++refused;
            }
        for (const auto& value : negative["ratios"].toArray())
            try
            {
                replaceExistingTextLines(source, 0, block.occurrence, fixed["english"].toString(),
                                         value.toDouble());
            }
            catch (const std::exception&)
            {
                ++refused;
            }
        try
        {
            replaceExistingTextLines(source, 0, block.occurrence, fixed["english"].toString(),
                                     ratio, {}, [] { return true; });
        }
        catch (const std::exception&)
        {
            ++refused;
        }
        check(encodePdf(source) == initial, "Line-count refusals and cancel preserve source");
    }
    check(refused == 10, "All line-count refusals executed");
    return {{"two_to_three_to_one", true}, {"Undo_Redo_save_reopen", true}, {"refusals", refused}};
}
QJsonObject testExistingTextLineCountUi(const QString& fixtures, const QString& output)
{
    const auto fixed = plan(fixtures);
    const auto source = readPdf(sourcePath(fixtures, fixed, "relative-leading"));
    const auto initial = encodePdf(source);
    ExistingTextDialog dialog(source, 0);
    dialog.show();
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    auto ready = [&]
    {
        check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
              "Line-count UI preview finished");
    };
    ready();
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    int row = -1;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == fixed["selected"].toString().left(60))
            row = i;
    check(row >= 0, "Line-count UI source selected");
    list->setCurrentRow(row);
    dialog.findChild<QComboBox*>("existingTextOperation")->setCurrentIndex(3);
    auto input = dialog.findChild<QPlainTextEdit*>("existingTextInput");
    input->setFocus();
    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(fixed["japanese"].toString());
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    auto family = dialog.findChild<QComboBox*>("existingTextFontFamily");
    family->setCurrentIndex(family->findData(fixed["explicit_family"].toString()));
    auto leading = dialog.findChild<QDoubleSpinBox*>("existingTextLeadingRatio");
    check(leading->isVisible() && leading->isEnabled(),
          "Line-count leading visible only in explicit mode");
    leading->setValue(fixed["leading_ratio"].toDouble());
    ready();
    auto apply = dialog.findChild<QPushButton*>("applyExistingText");
    check(apply->isEnabled(), "Line-count candidate ready");
    input->setFocus();
    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(fixed["refused"].toObject()["empty_line"].toString());
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    ready();
    check(!apply->isEnabled() &&
              dialog.findChild<QLabel*>("existingTextMessage")->text().contains("各行"),
          "Empty-line UI refuses apply and explains how to recover");
    check(encodePdf(source) == initial, "Invalid line-count preview preserves original");
    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(fixed["japanese"].toString());
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    ready();
    check(apply->isEnabled(), "Line-count UI recovers after refusal");
    check(dialog.grab().save(output + "/existing-text-line-count-ui.png"),
          "Actual line-count UI screenshot");
    check(encodePdf(source) == initial, "Line-count preview has not committed");
    apply->click();
    const auto candidate = dialog.takeDocument();
    retained(source, candidate, target(source, fixed["selected"].toString()).occurrence);
    writeCandidate(candidate, output + "/existing-text-line-count-ui.pdf");
    return {{"actual_three_line_paste_font_leading_apply", true}, {"native_IME", "未実行"}};
}
} // namespace tatsu
