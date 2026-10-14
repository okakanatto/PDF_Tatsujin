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
    const auto path = fixtures + "/existing-text-edit/styled-form-parts-v1/criteria.json";
    check(fileHash(path).toHex() ==
              "d7ae8793fb2d2e3e27f452885c75a005a30ab7a55ab2b18d452358302c6969e9",
          "Frozen styled Form criterion SHA");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read styled Form conditions");
    return QJsonDocument::fromJson(file.readAll()).object();
}
PDFDocument source(const QString& fixtures, const QJsonObject& fixed, const QString& name)
{
    const auto path = fixtures + "/existing-text-edit/styled-form-parts-v1/" + name + ".pdf";
    check(fileHash(path).toHex() == fixed["files"].toObject()[name + ".pdf"].toString().toLatin1(),
          "Frozen styled Form input SHA");
    return readPdf(path);
}
ExistingTextBlock selected(const PDFDocument& document, const QString& text, int invocation = 0)
{
    for (const auto& part : existingStyledTextParts(document, 0, {}, true))
        if (part.depth == 2 && part.text == text && invocation-- == 0)
            return part;
    fail("Styled Form part missing: " + text);
}
void retained(const PDFDocument& original, const PDFDocument& candidate, int occurrence)
{
    for (int page = 0; page < int(original.getCatalog()->getPageCount()); ++page)
    {
        const auto before = existingStyledTextParts(original, page, {}, true);
        const auto after = existingStyledTextParts(candidate, page, {}, true);
        for (const auto& part : before)
            if (page != 0 || part.occurrence != occurrence)
            {
                auto found = std::find_if(after.begin(), after.end(), [&](const auto& actual)
                                          { return actual.occurrence == part.occurrence; });
                check(found != after.end() && found->text == part.text &&
                          found->physical == part.physical && found->font == part.font &&
                          found->depth == part.depth,
                      "All unselected calls, pages, positions and fonts unchanged");
            }
    }
}
void ready(ExistingTextDialog& dialog)
{
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
          "Styled Form preview ready");
}
void choose(ExistingTextDialog& dialog, const QString& text, int invocation = 0)
{
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text().startsWith(text + "\n（グループ内・2階層）") && invocation-- == 0)
        {
            list->setCurrentRow(i);
            ready(dialog);
            return;
        }
    fail("Styled Form UI call missing");
}
void modes(ExistingTextDialog& dialog, bool groupsFirst)
{
    auto groups = dialog.findChild<QCheckBox*>("existingTextIncludeGroups");
    auto parts = dialog.findChild<QCheckBox*>("existingTextStyledParts");
    check(!groups->isChecked() && !parts->isChecked(), "Both extended modes explicit opt-in");
    (groupsFirst ? groups : parts)->setChecked(true);
    ready(dialog);
    (groupsFirst ? parts : groups)->setChecked(true);
    ready(dialog);
    check(groups->isEnabled() && groups->isChecked() && parts->isChecked(),
          "Both mode orders retain group selection");
}
} // namespace
QJsonObject testExistingStyledForms(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    int outputs = 0;
    for (const auto& value : fixed["positives"].toArray())
    {
        const auto name = value.toString();
        const auto original = source(fixtures, fixed, name);
        const auto before = encodePdf(original);
        for (const auto& part : existingStyledTextParts(original, 0))
            check(part.depth == 0, "Default styled mode excludes Forms");
        const auto replacements = fixed["replacements"].toObject();
        for (auto it = replacements.begin(); it != replacements.end(); ++it)
        {
            const auto part = selected(original, it.key());
            check(part.restriction.isEmpty(),
                  "Styled Form source part editable: " + part.restriction);
            const auto candidate = editExistingStyledTextPart(original, 0, part.occurrence,
                                                              ExistingTextChange::Replace,
                                                              it.value().toString(), {}, true);
            retained(original, candidate, part.occurrence);
            check(selected(candidate, it.value().toString()).font == part.font,
                  "Original standard, bold and Japanese fonts retained");
            const QString slug = it.key().startsWith("Intro")    ? "intro"
                                 : it.key().startsWith("Styled") ? "bold"
                                 : it.key().startsWith("Suffix") ? "suffix"
                                                                 : "japanese";
            writeCandidate(candidate, output + "/styled-form-" + name + "-" + slug + ".pdf");
            ++outputs;
        }
        const auto part = selected(original, fixed["remove"].toString());
        check(part.font == "Helvetica-Bold", "Actual owning Form bold font displayed");
        const auto removed = editExistingStyledTextPart(original, 0, part.occurrence,
                                                        ExistingTextChange::Remove, {}, {}, true);
        retained(original, removed, part.occurrence);
        writeCandidate(removed, output + "/styled-form-" + name + "-removed.pdf");
        ++outputs;
        const auto second = selected(original, fixed["remove"].toString(), 1);
        const auto changedSecond = editExistingStyledTextPart(
            original, 0, second.occurrence, ExistingTextChange::Replace, "Bold text", {}, true);
        retained(original, changedSecond, second.occurrence);
        writeCandidate(changedSecond, output + "/styled-form-" + name + "-second.pdf");
        ++outputs;
        Document doc;
        doc.open(fixtures + "/existing-text-edit/styled-form-parts-v1/" + name + ".pdf");
        const auto cursor = doc.cursor;
        const auto changed = editExistingStyledTextPart(
            original, 0, part.occurrence, ExistingTextChange::Replace, "Bold text", {}, true);
        doc.commit(changed);
        check(doc.cursor == cursor + 1, "One styled Form Undo unit");
        doc.undo();
        check(encodePdf(doc.pdf()) == before, "Styled Form Undo exact source");
        doc.redo();
        check(encodePdf(doc.pdf()) == encodePdf(changed), "Styled Form Redo exact candidate");
        doc.save(output + "/styled-form-" + name + "-saved.pdf");
        ++outputs;
        Document reopened;
        reopened.open(output + "/styled-form-" + name + "-saved.pdf");
        const auto saved = selected(reopened.pdf(), "Bold text");
        reopened.commit(editExistingStyledTextPart(reopened.pdf(), 0, saved.occurrence,
                                                   ExistingTextChange::Replace,
                                                   fixed["reedited"].toString(), {}, true));
        retained(original, reopened.pdf(), part.occurrence);
        reopened.save(output + "/styled-form-" + name + "-reedited.pdf");
        ++outputs;
        check(encodePdf(original) == before, "Styled Form snapshot immutable");
    }
    return {{"outputs", outputs},
            {"shared_nested_call_and_other_page_isolation", true},
            {"Undo_Redo_save_reopen_reedit", true}};
}
QJsonObject testExistingStyledFormsRefusals(const QString& fixtures, const QString&)
{
    const auto fixed = criteria(fixtures);
    int refusals = 0;
    auto reject = [&](const std::function<void()>& action, const QString& reason = QString())
    {
        bool failed = false;
        try
        {
            action();
        }
        catch (const std::exception& error)
        {
            check(reason.isEmpty() || QString::fromUtf8(error.what()).contains(reason),
                  "Styled Form refusal reason: " + QString::fromUtf8(error.what()));
            failed = true;
        }
        check(failed, "Styled Form unsupported operation refused");
        ++refusals;
    };
    for (const auto& value : fixed["refused"].toArray())
    {
        const auto name = value.toString().chopped(4);
        const auto pdf = source(fixtures, fixed, name);
        const auto before = encodePdf(pdf);
        reject([&] { existingStyledTextParts(pdf, 0, {}, true); });
        check(encodePdf(pdf) == before, "Unsupported Form parse atomic");
    }
    const auto clipped = source(fixtures, fixed, "bbox-refused");
    const auto clippedPart = selected(clipped, "Styled phrase");
    check(clippedPart.restriction.contains("表示範囲"), "Part-specific BBox restriction visible");
    reject(
        [&]
        {
            editExistingStyledTextPart(clipped, 0, clippedPart.occurrence,
                                       ExistingTextChange::Remove, {}, {}, true);
        },
        "表示範囲");
    const auto pdf = source(fixtures, fixed, "shared");
    const auto before = encodePdf(pdf);
    const auto part = selected(pdf, "Styled phrase");
    reject(
        [&]
        {
            editExistingStyledTextPart(pdf, 0, part.occurrence, ExistingTextChange::Replace,
                                       fixed["overflow"].toString(), {}, true);
        },
        "表示範囲");
    reject(
        [&]
        {
            editExistingStyledTextPart(pdf, 0, part.occurrence, ExistingTextChange::Replace,
                                       "未収録漢字", {}, true);
        });
    int stages = 0;
    editExistingStyledTextPart(
        pdf, 0, part.occurrence, ExistingTextChange::Replace, "Bold text",
        [&]
        {
            ++stages;
            return false;
        },
        true);
    check(stages > 10, "Form traversal and candidate cancellation checkpoints exercised");
    for (int stage = 1; stage <= stages; ++stage)
    {
        int current = 0;
        reject(
            [&]
            {
                editExistingStyledTextPart(
                    pdf, 0, part.occurrence, ExistingTextChange::Replace, "Bold text",
                    [&] { return ++current >= stage; }, true);
            });
        check(encodePdf(pdf) == before, "Every Form cancellation stage atomic");
    }
    return {{"refusals", refusals}, {"all_cancel_stages", stages}, {"source_immutable", true}};
}
QJsonObject testExistingStyledFormsUi(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    Window window;
    source(fixtures, fixed, "shared");
    window.doc.open(fixtures + "/existing-text-edit/styled-form-parts-v1/shared.pdf");
    window.doc.putSignature(1, "未選択の署名", {30, 30}, 10, Qt::black);
    window.refresh(true);
    window.show();
    writeCandidate(window.doc.pdf(), output + "/styled-form-ui-source.pdf");
    const auto original = window.doc.pdf();
    const auto before = encodePdf(original);
    const auto part = selected(original, "Styled phrase", 1);
    const auto cursor = window.doc.cursor;
    QTimer timer;
    bool handled = false;
    QString error;
    QObject::connect(&timer, &QTimer::timeout,
                     [&]
                     {
                         auto dialog =
                             dynamic_cast<ExistingTextDialog*>(QApplication::activeModalWidget());
                         if (!dialog || handled)
                             return;
                         handled = true;
                         try
                         {
                             ready(*dialog);
                             modes(*dialog, true);
                             choose(*dialog, "Styled phrase", 1);
                             auto input = dialog->findChild<QPlainTextEdit*>("existingTextInput");
                             input->setFocus();
                             QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
                             QTest::keyClicks(input, "Bold text");
                             ready(*dialog);
                             auto apply = dialog->findChild<QPushButton*>("applyExistingText");
                             check(apply->isEnabled(),
                                   dialog->findChild<QLabel*>("existingTextMessage")->text());
                             check(dialog->grab().save(output + "/styled-form-ui.png"),
                                   "Styled Form screenshot");
                             dialog->resize(800, 480);
                             QApplication::processEvents();
                             check(dialog->grab().save(output + "/styled-form-ui-compact.png"),
                                   "Compact Form screenshot");
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
    check(handled && error.isEmpty(), "Styled Form menu: " + error);
    check(window.doc.cursor == cursor + 1, "Styled Form menu one Undo");
    retained(original, window.doc.pdf(), part.occurrence);
    window.doc.save(output + "/styled-form-ui.pdf");
    const auto edited = encodePdf(window.doc.pdf());
    window.doc.undo();
    check(encodePdf(window.doc.pdf()) == before, "Undo retains unsaved signature");
    window.doc.redo();
    check(encodePdf(window.doc.pdf()) == edited, "Redo exact Form candidate");
    ExistingTextDialog recover(window.doc.pdf(), 0);
    recover.show();
    ready(recover);
    modes(recover, false);
    choose(recover, "Bold text");
    auto input = recover.findChild<QPlainTextEdit*>("existingTextInput");
    auto apply = recover.findChild<QPushButton*>("applyExistingText");
    input->setPlainText(fixed["overflow"].toString());
    ready(recover);
    check(!apply->isEnabled() &&
              recover.findChild<QLabel*>("existingTextMessage")->text().contains("表示範囲"),
          "Form BBox rejection disables apply with reason");
    input->setPlainText(fixed["reedited"].toString());
    ready(recover);
    check(apply->isEnabled(), "Form rejection recovers with current candidate");
    writeCandidate(recover.takeDocument(), output + "/styled-form-ui-recovered.pdf");
    recover.reject();
    check(encodePdf(window.doc.pdf()) == edited, "Cancel candidate does not commit");
    return {{"actual_menu_second_call", true},
            {"mode_orders_BBox_recovery_cancel", true},
            {"unsaved_signature_Undo_Redo", true},
            {"native_IME_OS_clipboard_DPI", "未実行"}};
}
} // namespace tatsu
