#include "existing_text_dialog.h"
#include "existing_text_tests.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
struct FlowingTests
{
    bool multiline;
    ExistingTextPartLayout flow;
    QString folder, prefix, intro, bold, suffix;
    explicit FlowingTests(bool lines)
        : multiline(lines),
          flow(lines ? ExistingTextPartLayout::FlowingLines : ExistingTextPartLayout::FlowingLine),
          folder(lines ? "flowing-lines-v1" : "flowing-parts-v1"),
          prefix(lines ? "flowing-lines-" : "flowing-"), intro(lines ? "Second " : "Intro "),
          bold(lines ? "Bold second" : "Styled phrase"),
          suffix(lines ? "Suffix second" : "Suffix intact")
    {
    }

    void check(bool value, const QString& message)
    {
        if (!value)
            fail(message);
    }
    QJsonObject criteria(const QString& fixtures)
    {
        const auto path = fixtures + "/existing-text-edit/" + folder + "/criteria.json";
        check(fileHash(path).toHex() ==
                  (multiline ? "7d30fb24b74b61ffd35558f513d68a87d2b21a7871a86eb69161ccb97a629bd7"
                             : "1ac4f62edf2325101e0c8d0230a339518d6ae449eafd74fec2f5351fea968818"),
              "Frozen flowing parts criterion SHA");
        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "Read flowing parts conditions");
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    PDFDocument source(const QString& fixtures, const QJsonObject& fixed, const QString& name)
    {
        const auto path = fixtures + "/existing-text-edit/" + folder + "/" + name + ".pdf";
        check(fileHash(path).toHex() ==
                  fixed["files"].toObject()[name + ".pdf"].toString().toLatin1(),
              "Frozen flowing parts input SHA");
        return readPdf(path);
    }
    ExistingTextBlock selected(const PDFDocument& document, const QString& text, int invocation = 0,
                               int depth = 2)
    {
        for (const auto& part : existingStyledTextParts(document, 0, {}, true, flow))
            if (part.depth == depth && part.text == text && invocation-- == 0)
                return part;
        fail("Flowing part missing: " + text);
    }
    void retained(const PDFDocument& original, const PDFDocument& candidate, int first)
    {
        for (int page = 0; page < int(original.getCatalog()->getPageCount()); ++page)
        {
            const auto before = existingStyledTextParts(original, page, {}, true, flow);
            const auto after = existingStyledTextParts(candidate, page, {}, true, flow);
            for (const auto& part : before)
                if (page != 0 || part.occurrence < first || part.occurrence >= first + 4)
                {
                    const auto found =
                        std::find_if(after.begin(), after.end(), [&](const auto& actual)
                                     { return actual.occurrence == part.occurrence; });
                    check(found != after.end() && found->text == part.text &&
                              found->physical == part.physical && found->font == part.font &&
                              found->depth == part.depth,
                          "Other blocks, calls and pages exactly retained");
                }
        }
    }
    void ready(ExistingTextDialog& dialog)
    {
        auto preview =
            dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
        check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
              "Flowing preview ready");
    }
    void modes(ExistingTextDialog& dialog, bool groupsFirst)
    {
        auto groups = dialog.findChild<QCheckBox*>("existingTextIncludeGroups");
        auto parts = dialog.findChild<QCheckBox*>("existingTextStyledParts");
        auto layout = dialog.findChild<QComboBox*>("existingTextPartLayout");
        check(!parts->isChecked() && !groups->isChecked() && layout->currentIndex() == 0,
              "Positioned mode remains default");
        (groupsFirst ? groups : parts)->setChecked(true);
        ready(dialog);
        (groupsFirst ? parts : groups)->setChecked(true);
        ready(dialog);
        check(layout->isVisible() && layout->itemText(multiline ? 2 : 1).contains("後続"),
              "Explicit flowing choice");
        layout->setCurrentIndex(multiline ? 2 : 1);
        ready(dialog);
        check(parts->isChecked() && groups->isChecked(), "Mode order preserves group opt-in");
    }
    void choose(ExistingTextDialog& dialog, int invocation)
    {
        auto list = dialog.findChild<QListWidget*>("existingTextList");
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text().startsWith(bold + "\n（グループ内・2階層）") &&
                invocation-- == 0)
            {
                list->setCurrentRow(i);
                ready(dialog);
                if (multiline)
                    check(dialog.findChild<QLabel*>("existingTextFontInfo")
                              ->text()
                              .contains("本文の2行目"),
                          "Selected physical line is distinct from part text length");
                return;
            }
        fail("Flowing UI call missing");
    }
    void outlines(ExistingTextDialog& dialog, const PDFDocument& original)
    {
        const auto rows = existingStyledTextParts(original, 0, {}, true, flow);
        const auto candidate = existingStyledTextParts(dialog.takeDocument(), 0, {}, true, flow);
        auto preview =
            dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
        int expected = 0;
        for (int i = 0; i < rows.size(); ++i)
        {
            const auto found =
                std::find_if(candidate.begin(), candidate.end(), [&](const auto& part)
                             { return part.occurrence == rows[i].occurrence; });
            if (found == candidate.end() || !found->physical.isValid())
                continue;
            ++expected;
            check(preview->regions.contains(qMakePair(i, found->physical)),
                  "Every preview outline uses current candidate occurrence and shifted bounds");
        }
        check(preview->regions.size() == expected, "Removed part has no stale outline");
    }
    QJsonObject testExistingFlowingParts(const QString& fixtures, const QString& output)
    {
        const auto fixed = criteria(fixtures);
        const auto positives = fixed["positives"].toObject();
        int outputs = 0;
        for (auto variant = positives.begin(); variant != positives.end(); ++variant)
        {
            const auto name = variant.key();
            const int depth = variant.value().toInt();
            const auto original = source(fixtures, fixed, name);
            const auto before = encodePdf(original);
            const auto first = selected(original, intro, 0, depth).occurrence;
            const auto replacements = fixed["replacements"].toObject();
            for (auto it = replacements.begin(); it != replacements.end(); ++it)
            {
                const auto part = selected(original, it.key(), 0, depth);
                check(part.restriction.isEmpty(), "Flowing part supported: " + part.restriction);
                const auto candidate = editExistingStyledTextPart(
                    original, 0, part.occurrence, ExistingTextChange::Replace,
                    it.value().toString(), {}, true, flow);
                retained(original, candidate, first);
                check(selected(candidate, it.value().toString(), 0, depth).font == part.font,
                      "Original font retained");
                if (it.key() == bold)
                    check(selected(candidate, suffix, 0, depth).physical !=
                              selected(original, suffix, 0, depth).physical,
                          "Following suffix actually moves when bold advance changes");
                const QString slug = it.key() == intro    ? "intro"
                                     : it.key() == bold   ? "bold"
                                     : it.key() == suffix ? "suffix"
                                                          : "japanese";
                writeCandidate(candidate, output + "/" + prefix + name + "-" + slug + ".pdf");
                ++outputs;
            }
            const auto part = selected(original, fixed["remove"].toString(), 0, depth);
            const auto removed = editExistingStyledTextPart(
                original, 0, part.occurrence, ExistingTextChange::Remove, {}, {}, true, flow);
            retained(original, removed, first);
            check(selected(removed, suffix, 0, depth).physical !=
                      selected(original, suffix, 0, depth).physical,
                  "Removal closes selected advance only");
            writeCandidate(removed, output + "/" + prefix + name + "-removed.pdf");
            ++outputs;
            if (depth)
            {
                const auto second = selected(original, bold, 1, depth);
                const auto secondFirst = selected(original, intro, 1, depth).occurrence;
                const auto candidate = editExistingStyledTextPart(original, 0, second.occurrence,
                                                                  ExistingTextChange::Replace,
                                                                  "Bold text", {}, true, flow);
                retained(original, candidate, secondFirst);
                writeCandidate(candidate, output + "/" + prefix + name + "-second.pdf");
                ++outputs;
            }
            if (multiline)
            {
                for (const auto& edge : {qMakePair(QString("Intro "), QString("First ")),
                                         qMakePair(QString("Bold third"), QString("Last text"))})
                {
                    const auto rows = existingStyledTextParts(original, 0, {}, true, flow);
                    const auto found =
                        std::find_if(rows.begin(), rows.end(), [&](const auto& row)
                                     { return row.depth == depth && row.text == edge.first; });
                    check(found != rows.end(), "Initial and last physical line found");
                    const auto changed = editExistingStyledTextPart(original, 0, found->occurrence,
                                                                    ExistingTextChange::Replace,
                                                                    edge.second, {}, true, flow);
                    const auto lineStart =
                        found->occurrence - (found->text == "Bold third" ? 1 : 0);
                    retained(original, changed, lineStart);
                    writeCandidate(changed,
                                   output + "/" + prefix + name +
                                       (found->physicalLine == 0 ? "-first.pdf" : "-last.pdf"));
                    ++outputs;
                }
            }
            Document doc;
            doc.open(fixtures + "/existing-text-edit/" + folder + "/" + name + ".pdf");
            const auto cursor = doc.cursor;
            const auto changed = editExistingStyledTextPart(original, 0, part.occurrence,
                                                            ExistingTextChange::Replace,
                                                            "Bold text", {}, true, flow);
            doc.commit(changed);
            check(doc.cursor == cursor + 1, "Flowing edit one Undo");
            doc.undo();
            check(encodePdf(doc.pdf()) == before, "Flowing Undo exact source");
            doc.redo();
            check(encodePdf(doc.pdf()) == encodePdf(changed), "Flowing Redo exact candidate");
            doc.save(output + "/" + prefix + name + "-saved.pdf");
            ++outputs;
            Document reopened;
            reopened.open(output + "/" + prefix + name + "-saved.pdf");
            const auto saved = selected(reopened.pdf(), "Bold text", 0, depth);
            reopened.commit(editExistingStyledTextPart(
                reopened.pdf(), 0, saved.occurrence, ExistingTextChange::Replace,
                fixed["reedited"].toString(), {}, true, flow));
            retained(original, reopened.pdf(), first);
            reopened.save(output + "/" + prefix + name + "-reedited.pdf");
            ++outputs;
            check(encodePdf(original) == before, "Flowing source immutable");
        }
        return {{"outputs", outputs},
                {"following_parts_move", true},
                {"other_calls_pages_fonts_retained", true},
                {"Undo_Redo_save_reopen_reedit", true}};
    }
    QJsonObject testExistingFlowingRefusals(const QString& fixtures, const QString&)
    {
        const auto fixed = criteria(fixtures);
        int refusals = 0;
        auto reject = [&](const std::function<void()>& action)
        {
            bool failed = false;
            try
            {
                action();
            }
            catch (const std::exception&)
            {
                failed = true;
            }
            check(failed, "Unsupported flowing edit refused");
            ++refusals;
        };
        for (const auto& value : fixed["refused"].toArray())
        {
            const auto pdf = source(fixtures, fixed, value.toString());
            const auto bytes = encodePdf(pdf);
            reject([&] { existingStyledTextParts(pdf, 0, {}, true, flow); });
            check(encodePdf(pdf) == bytes, "Flowing inspection refusal atomic");
        }
        for (const auto& value : fixed["restricted"].toArray())
        {
            const auto name = value.toString();
            const auto pdf = source(fixtures, fixed, name);
            const auto bytes = encodePdf(pdf);
            const auto part = selected(pdf, bold, 0, name == "bbox-refused" ? 2 : 0);
            check(!part.restriction.isEmpty(), "Flowing line restriction shown");
            reject(
                [&]
                {
                    editExistingStyledTextPart(pdf, 0, part.occurrence, ExistingTextChange::Remove,
                                               {}, {}, true, flow);
                });
            check(encodePdf(pdf) == bytes, "Flowing restricted removal atomic");
        }
        for (const QString name : {"page", "shared"})
        {
            const auto pdf = source(fixtures, fixed, name);
            const auto bytes = encodePdf(pdf);
            const auto part = selected(pdf, bold, 0, name == "page" ? 0 : 2);
            reject(
                [&]
                {
                    editExistingStyledTextPart(
                        pdf, 0, part.occurrence, ExistingTextChange::Replace,
                        fixed[name == "page" ? "page_overflow" : "form_overflow"].toString(), {},
                        true, flow);
                });
            reject(
                [&]
                {
                    editExistingStyledTextPart(pdf, 0, part.occurrence, ExistingTextChange::Replace,
                                               "未収録漢字", {}, true, flow);
                });
            reject(
                [&]
                {
                    editExistingStyledTextPart(pdf, 0, part.occurrence, ExistingTextChange::Replace,
                                               "Bold text", {}, true);
                });
            check(encodePdf(pdf) == bytes, "Overflow, font and default mode refusals atomic");
        }
        const auto pdf = source(fixtures, fixed, "shared");
        const auto before = encodePdf(pdf);
        const auto part = selected(pdf, bold);
        int stages = 0;
        editExistingStyledTextPart(
            pdf, 0, part.occurrence, ExistingTextChange::Replace, "Bold text",
            [&]
            {
                ++stages;
                return false;
            },
            true, flow);
        check(stages > 10, "Flowing traversal cancellation stages");
        for (int stage = 1; stage <= stages; ++stage)
        {
            int current = 0;
            reject(
                [&]
                {
                    editExistingStyledTextPart(
                        pdf, 0, part.occurrence, ExistingTextChange::Replace, "Bold text",
                        [&] { return ++current >= stage; }, true, flow);
                });
            check(encodePdf(pdf) == before, "Every flowing cancellation atomic");
        }
        return {{"refusals", refusals}, {"all_cancel_stages", stages}, {"source_immutable", true}};
    }
    QJsonObject testExistingFlowingUi(const QString& fixtures, const QString& output)
    {
        const auto fixed = criteria(fixtures);
        source(fixtures, fixed, "shared");
        Window window;
        window.doc.open(fixtures + "/existing-text-edit/" + folder + "/shared.pdf");
        window.doc.putSignature(1, "未選択の署名", {30, 30}, 10, Qt::black);
        window.refresh(true);
        window.show();
        const auto original = window.doc.pdf();
        const auto before = encodePdf(original);
        writeCandidate(original, output + "/" + prefix + "ui-source.pdf");
        const int first = selected(original, intro, 1).occurrence;
        const auto cursor = window.doc.cursor;
        QTimer timer;
        bool handled = false;
        QString error;
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
                    modes(*dialog, true);
                    choose(*dialog, 1);
                    auto input = dialog->findChild<QPlainTextEdit*>("existingTextInput");
                    input->setFocus();
                    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
                    QTest::keyClicks(input, "Bold text");
                    ready(*dialog);
                    auto apply = dialog->findChild<QPushButton*>("applyExistingText");
                    check(apply->isEnabled(),
                          dialog->findChild<QLabel*>("existingTextMessage")->text());
                    outlines(*dialog, original);
                    check(dialog->grab().save(output + "/" + prefix + "ui.png"),
                          "Flowing UI screenshot");
                    dialog->resize(800, 480);
                    QApplication::processEvents();
                    check(dialog->grab().save(output + "/" + prefix + "ui-compact.png"),
                          "Compact flowing UI screenshot");
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
        check(handled && error.isEmpty(), "Actual flowing menu: " + error);
        check(window.doc.cursor == cursor + 1, "Flowing menu one Undo");
        retained(original, window.doc.pdf(), first);
        window.doc.save(output + "/" + prefix + "ui-replaced.pdf");
        const auto edited = encodePdf(window.doc.pdf());
        window.doc.undo();
        check(encodePdf(window.doc.pdf()) == before, "Flowing Undo retains unsaved signature");
        window.doc.redo();
        check(encodePdf(window.doc.pdf()) == edited, "Flowing Redo exact");
        ExistingTextDialog remove(original, 0);
        remove.show();
        ready(remove);
        modes(remove, false);
        choose(remove, 1);
        auto input = remove.findChild<QPlainTextEdit*>("existingTextInput");
        auto apply = remove.findChild<QPushButton*>("applyExistingText");
        input->setPlainText(fixed["form_overflow"].toString());
        ready(remove);
        check(!apply->isEnabled() &&
                  remove.findChild<QLabel*>("existingTextMessage")->text().contains("表示範囲"),
              "Following glyph BBox overflow disabled with reason");
        input->setPlainText("Bold text");
        ready(remove);
        check(apply->isEnabled(), "Flowing rejection recovers");
        remove.findChild<QComboBox*>("existingTextOperation")->setCurrentIndex(2);
        ready(remove);
        check(!apply->isEnabled(), "Removal requires explicit consent");
        remove.findChild<QCheckBox*>("existingTextDeleteConsent")->setChecked(true);
        check(apply->isEnabled(), "Supported flowing removal enabled");
        outlines(remove, original);
        writeCandidate(remove.takeDocument(), output + "/" + prefix + "ui-removed.pdf");
        remove.reject();
        check(encodePdf(window.doc.pdf()) == edited, "Cancelled candidate leaves owner unchanged");
        bool stale = false;
        ExistingTextDialog guarded(original, 0,
                                   [&]
                                   {
                                       if (stale)
                                           fail("stale snapshot");
                                   });
        guarded.show();
        ready(guarded);
        modes(guarded, false);
        choose(guarded, 0);
        guarded.findChild<QPlainTextEdit*>("existingTextInput")->setPlainText("Bold text");
        ready(guarded);
        stale = true;
        guarded.accept();
        check(!guarded.findChild<QPushButton*>("applyExistingText")->isEnabled(),
              "Stale candidate cannot apply");
        guarded.reject();
        return {{"actual_menu_second_call", true},
                {"shifted_and_removed_preview_outlines", true},
                {"recovery_cancel_stale_Undo_Redo_signature", true},
                {"native_IME_OS_clipboard_DPI", "未実行"}};
    }
};
} // namespace
QJsonObject testExistingFlowingParts(const QString& fixtures, const QString& output)
{
    return FlowingTests(false).testExistingFlowingParts(fixtures, output);
}
QJsonObject testExistingFlowingRefusals(const QString& fixtures, const QString& output)
{
    return FlowingTests(false).testExistingFlowingRefusals(fixtures, output);
}
QJsonObject testExistingFlowingUi(const QString& fixtures, const QString& output)
{
    return FlowingTests(false).testExistingFlowingUi(fixtures, output);
}
QJsonObject testExistingFlowingLines(const QString& fixtures, const QString& output)
{
    return FlowingTests(true).testExistingFlowingParts(fixtures, output);
}
QJsonObject testExistingFlowingLinesRefusals(const QString& fixtures, const QString& output)
{
    return FlowingTests(true).testExistingFlowingRefusals(fixtures, output);
}
QJsonObject testExistingFlowingLinesUi(const QString& fixtures, const QString& output)
{
    return FlowingTests(true).testExistingFlowingUi(fixtures, output);
}
} // namespace tatsu
