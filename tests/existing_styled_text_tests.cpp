#include "existing_text_dialog.h"
#include "existing_text_tests.h"
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
QJsonObject criteria(const QString& fixtures)
{
    const auto path = fixtures + "/existing-text-edit/styled-parts-v1/criteria.json";
    check(fileHash(path).toHex() ==
              "76781b4b06150f72fcc94d5cf9b693cd4c2485e0a58e0ba78549df5348e1aab9",
          "Frozen styled-part criterion SHA");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read styled-part conditions");
    return QJsonDocument::fromJson(file.readAll()).object();
}
ExistingTextBlock selected(const PDFDocument& document, const QString& text)
{
    for (const auto& part : existingStyledTextParts(document, 0))
        if (part.text == text)
            return part;
    fail("Styled part missing: " + text);
}
void retained(const PDFDocument& source, const PDFDocument& candidate, int occurrence)
{
    const auto before = existingStyledTextParts(source, 0);
    const auto after = existingStyledTextParts(candidate, 0);
    for (const auto& part : before)
        if (part.occurrence != occurrence)
        {
            const auto actual = selected(candidate, part.text);
            check(actual.physical == part.physical && actual.font == part.font,
                  "Styled-part edit preserves other glyph positions and font states");
        }
    check(after.size() == before.size() || after.size() + 1 == before.size(),
          "Only selected part may be removed");
}
PageRegionPreview* ready(ExistingTextDialog& dialog)
{
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
          "Styled-part worker completed");
    return preview;
}
void choose(ExistingTextDialog& dialog, const QString& text)
{
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == text)
        {
            list->setCurrentRow(i);
            ready(dialog);
            return;
        }
    fail("Styled UI part missing: " + text);
}
} // namespace
QJsonObject testExistingStyledText(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    int checked = 0;
    for (const auto& name : {QString("absolute"), QString("rotated")})
    {
        const auto path = fixtures + "/existing-text-edit/styled-parts-v1/" + name + ".pdf";
        check(fileHash(path).toHex() ==
                  fixed["files"].toObject()[name + ".pdf"].toString().toLatin1(),
              "Frozen styled-part input SHA");
        const auto source = readPdf(path);
        const auto original = encodePdf(source);
        const auto replacements = fixed["replacements"].toObject();
        for (auto it = replacements.begin(); it != replacements.end(); ++it)
        {
            const auto part = selected(source, it.key());
            check(part.restriction.isEmpty(), "Explicit-position styled part editable");
            const auto changed = editExistingStyledTextPart(
                source, 0, part.occurrence, ExistingTextChange::Replace, it.value().toString());
            retained(source, changed, part.occurrence);
            check(selected(changed, it.value().toString()).font == part.font,
                  "Selected font preserved, including bold and Japanese subset");
            const QString slug = it.key().startsWith("Intro")    ? "intro"
                                 : it.key().startsWith("Styled") ? "bold"
                                 : it.key().startsWith("Suffix") ? "suffix"
                                                                 : "japanese";
            writeCandidate(changed, output + "/styled-" + name + "-" + slug + ".pdf");
            ++checked;
        }
        const auto part = selected(source, fixed["remove"].toString());
        check(part.font == "Helvetica-Bold",
              "Styled font display uses actual BaseFont rather than resource alias");
        const auto removed =
            editExistingStyledTextPart(source, 0, part.occurrence, ExistingTextChange::Remove);
        retained(source, removed, part.occurrence);
        writeCandidate(removed, output + "/styled-" + name + "-removed.pdf");
        ++checked;
        Document history;
        history.open(path);
        const auto changed = editExistingStyledTextPart(source, 0, part.occurrence,
                                                        ExistingTextChange::Replace, "Bold text");
        const auto cursor = history.cursor;
        history.commit(changed);
        check(history.cursor == cursor + 1, "Styled part is one Undo unit");
        history.undo();
        check(encodePdf(history.pdf()) == original, "Styled part Undo exact source");
        history.redo();
        check(encodePdf(history.pdf()) == encodePdf(changed), "Styled part Redo exact candidate");
        history.save(output + "/styled-" + name + "-saved.pdf");
        Document reopened;
        reopened.open(output + "/styled-" + name + "-saved.pdf");
        const auto saved = selected(reopened.pdf(), "Bold text");
        reopened.commit(editExistingStyledTextPart(reopened.pdf(), 0, saved.occurrence,
                                                   ExistingTextChange::Replace,
                                                   fixed["reedited"].toString()));
        retained(source, reopened.pdf(), part.occurrence);
        reopened.save(output + "/styled-" + name + "-reedited.pdf");
        ++checked;
        check(fileHash(path).toHex() ==
                      fixed["files"].toObject()[name + ".pdf"].toString().toLatin1() &&
                  encodePdf(source) == original,
              "Original file and snapshot unchanged");
    }
    return {{"outputs", checked},
            {"mixed_font_original_positions", true},
            {"Undo_Redo_save_reopen_reedit", true}};
}
QJsonObject testExistingStyledTextRefusals(const QString& fixtures, const QString&)
{
    const auto fixed = criteria(fixtures);
    int refused = 0;
    auto reject = [&](const std::function<void()>& action)
    {
        bool rejected = false;
        try
        {
            action();
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        check(rejected, "Unsupported styled-part operation refused");
        ++refused;
    };
    const auto negative = fixed["refusals"].toObject();
    for (auto it = negative.begin(); it != negative.end(); ++it)
    {
        const auto path = fixtures + "/existing-text-edit/styled-parts-v1/" + it.key();
        check(fileHash(path).toHex() == fixed["files"].toObject()[it.key()].toString().toLatin1(),
              "Frozen styled refusal SHA");
        const auto source = readPdf(path);
        const auto before = encodePdf(source);
        const auto part = selected(source, it.value().toString());
        check(!part.restriction.isEmpty(), "Styled refusal has visible reason");
        reject(
            [&] {
                editExistingStyledTextPart(source, 0, part.occurrence, ExistingTextChange::Remove);
            });
        check(encodePdf(source) == before, "Refusal keeps original snapshot");
    }
    const auto source = readPdf(fixtures + "/existing-text-edit/styled-parts-v1/absolute.pdf");
    const auto before = encodePdf(source);
    const auto part = selected(source, "Styled phrase");
    for (const auto& text :
         {QString(), QString("two\nlines"), QString("未収録漢字"), QString(200, 'W')})
        reject(
            [&] {
                editExistingStyledTextPart(source, 0, part.occurrence, ExistingTextChange::Replace,
                                           text);
            });
    reject([&] { editExistingStyledTextPart(source, 0, -1, ExistingTextChange::Remove); });
    reject(
        [&]
        { editExistingStyledTextPart(source, -1, part.occurrence, ExistingTextChange::Remove); });
    reject(
        [&]
        { editExistingStyledTextPart(source, 0, part.occurrence, ExistingTextChange::Geometry); });
    reject(
        [&]
        {
            editExistingStyledTextPart(source, 0, part.occurrence, ExistingTextChange::Remove, {},
                                       [] { return true; });
        });
    int stages = 0;
    editExistingStyledTextPart(source, 0, part.occurrence, ExistingTextChange::Replace, "Bold text",
                               [&]
                               {
                                   ++stages;
                                   return false;
                               });
    int late = 0;
    reject(
        [&]
        {
            editExistingStyledTextPart(source, 0, part.occurrence, ExistingTextChange::Replace,
                                       "Bold text", [&] { return ++late >= stages - 1; });
        });
    const auto old = readPdf(fixtures + "/existing-text-edit/multiline/mixed-fonts.pdf");
    for (const auto& block : existingTextBlocks(old, 0))
        if (block.text == "First body line\nSecond body line")
        {
            check(!block.restriction.isEmpty(), "Prior paragraph mixed-font restriction retained");
            reject(
                [&] {
                    editExistingText(old, 0, block.occurrence, ExistingTextChange::Replace,
                                     "First\nSecond");
                });
        }
    check(encodePdf(source) == before, "All styled refusals atomic");
    return {{"refusals", refused}, {"prior_paragraph_contract_unchanged", true}};
}
QJsonObject testExistingStyledTextUi(const QString& fixtures, const QString& output)
{
    criteria(fixtures);
    Window window;
    window.doc.open(fixtures + "/existing-text-edit/styled-parts-v1/absolute.pdf");
    window.doc.putSignature(2, "未選択署名を保持", {30, 30}, 10, Qt::black);
    window.refresh(true);
    window.show();
    writeCandidate(window.doc.pdf(), output + "/styled-ui-source.pdf");
    const auto before = encodePdf(window.doc.pdf());
    const auto cursor = window.doc.cursor;
    bool handled = false;
    QString error;
    QTimer timer;
    QObject::connect(
        &timer, &QTimer::timeout,
        [&]
        {
            auto dialog = dynamic_cast<ExistingTextDialog*>(QApplication::activeModalWidget());
            if (!dialog || handled)
                return;
            handled = true;
            try
            {
                ready(*dialog);
                dialog->findChild<QCheckBox*>("existingTextStyledParts")->setChecked(true);
                ready(*dialog);
                choose(*dialog, "Styled phrase");
                check(!dialog->findChild<QComboBox*>("existingTextFontFamily")->isEnabled(),
                      "Styled UI preserves original font explicitly");
                auto input = dialog->findChild<QPlainTextEdit*>("existingTextInput");
                input->setFocus();
                QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClicks(input, "Bold text");
                ready(*dialog);
                auto apply = dialog->findChild<QPushButton*>("applyExistingText");
                check(apply->isEnabled(),
                      dialog->findChild<QLabel*>("existingTextMessage")->text());
                check(dialog->grab().save(output + "/styled-ui.png"),
                      "Styled UI actual candidate screenshot");
                dialog->resize(800, 480);
                QApplication::processEvents();
                check(dialog->grab().save(output + "/styled-ui-compact.png"),
                      "Compact styled UI screenshot");
                apply->click();
            }
            catch (const std::exception& exception)
            {
                error = QString::fromUtf8(exception.what());
                dialog->reject();
            }
        });
    timer.start(10);
    window.findChild<QAction*>("editExistingTextBlocks")->trigger();
    timer.stop();
    check(handled && error.isEmpty(), "Styled menu operation: " + error);
    check(window.doc.cursor == cursor + 1, "Styled menu apply one Undo");
    const auto edited = encodePdf(window.doc.pdf());
    window.doc.undo();
    check(encodePdf(window.doc.pdf()) == before, "Styled menu Undo preserves unsaved signature");
    window.doc.redo();
    check(encodePdf(window.doc.pdf()) == edited, "Styled menu Redo exact");
    window.doc.save(output + "/styled-ui.pdf");
    ExistingTextDialog invalid(window.doc.pdf(), 0);
    invalid.show();
    ready(invalid);
    invalid.findChild<QCheckBox*>("existingTextStyledParts")->setChecked(true);
    ready(invalid);
    choose(invalid, "Bold text");
    invalid.findChild<QPlainTextEdit*>("existingTextInput")->setPlainText(QString(200, 'W'));
    ready(invalid);
    check(!invalid.findChild<QPushButton*>("applyExistingText")->isEnabled(),
          "Invalid styled candidate cannot apply");
    invalid.reject();
    check(encodePdf(window.doc.pdf()) == edited, "Styled dialog cancel has no partial commit");
    ExistingTextDialog stale(window.doc.pdf(), 0, [] { fail("原本が外部更新されました。"); });
    stale.show();
    ready(stale);
    stale.findChild<QCheckBox*>("existingTextStyledParts")->setChecked(true);
    ready(stale);
    choose(stale, "Bold text");
    auto input = stale.findChild<QPlainTextEdit*>("existingTextInput");
    input->setPlainText(QString(200, 'W'));
    input->setPlainText("Bold edit");
    ready(stale);
    auto apply = stale.findChild<QPushButton*>("applyExistingText");
    check(apply->isEnabled() &&
              selected(stale.takeDocument(), "Bold edit").font == "Helvetica-Bold",
          "Latest styled generation replaces stale invalid candidate");
    apply->click();
    check(!apply->isEnabled() && stale.isVisible(), "External source update blocks styled apply");
    stale.reject();
    check(encodePdf(window.doc.pdf()) == edited, "Stale candidate never commits");
    return {{"actual_menu_select_preview_apply", true},
            {"one_Undo_unsaved_signature", true},
            {"invalid_cancel_noncommit", true},
            {"native_IME_OS_clipboard", "未実行"}};
}
} // namespace tatsu
