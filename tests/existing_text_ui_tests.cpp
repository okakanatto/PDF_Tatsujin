#include "existing_text_dialog.h"
#include "existing_text_tests.h"
#include "form_fields.h"
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
PageRegionPreview* ready(ExistingTextDialog& dialog)
{
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingTextPreview"));
    check(QTest::qWaitFor(
              [&] { return preview->property("renderReady").toBool() && !preview->image.isNull(); },
              15000),
          "Actual body candidate preview ready");
    return preview;
}
int row(ExistingTextDialog& dialog, const QString& text)
{
    auto list = dialog.findChild<QListWidget*>("existingTextList");
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == text)
            return i;
    fail("Fixed body row not present: " + text);
}
} // namespace
QJsonObject testExistingTextUi(const QString& fixtures, const QString& output)
{
    Window original;
    original.doc.open(fixtures + "/existing-text-edit/visible/body-text.pdf");
    original.doc.putSignature(2, "本文編集の範囲外署名", {30, 30}, 10, Qt::black);
    original.refresh(true);
    original.show();
    writeCandidate(original.doc.pdf(), output + "/existing-text-ui-source.pdf");
    const auto before = encodePdf(original.doc.pdf());
    const auto cursor = original.doc.cursor;
    for (const auto& pair :
         QVector<QPair<QString, QString>>{{"Original body line", "Edited body line"},
                                          {"日本語の本文を編集します", "本文の日本語を編集します"}})
    {
        bool handling = false;
        QString error;
        QTimer timer;
        QObject::connect(
            &timer, &QTimer::timeout,
            [&]
            {
                auto dialog = dynamic_cast<ExistingTextDialog*>(QApplication::activeModalWidget());
                if (!dialog || handling)
                    return;
                handling = true;
                try
                {
                    ready(*dialog);
                    dialog->findChild<QListWidget*>("existingTextList")
                        ->setCurrentRow(row(*dialog, pair.first));
                    ready(*dialog);
                    auto input = dialog->findChild<QPlainTextEdit*>("existingTextInput");
                    check(input->isEnabled(), "Supported visible body editable");
                    input->setFocus();
                    QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
                    if (pair.first.startsWith("Original"))
                        QTest::keyClicks(input, pair.second);
                    else
                    {
                        QApplication::clipboard()->setText(pair.second);
                        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
                    }
                    ready(*dialog);
                    check(input->toPlainText() == pair.second, "Actual edited body input");
                    auto apply = dialog->findChild<QPushButton*>("applyExistingText");
                    check(apply->isEnabled(), "Candidate ready before body Apply");
                    check(dialog->grab().save(
                              output + "/existing-text-ui-" +
                              (pair.first.startsWith("Original") ? "english" : "japanese") +
                              ".png"),
                          "Body UI screenshot");
                    dialog->resize(800, 480);
                    QApplication::processEvents();
                    check(dialog->grab().save(output + "/existing-text-ui-compact.png"),
                          "Compact body UI screenshot");
                    apply->click();
                }
                catch (const std::exception& exception)
                {
                    error = QString::fromUtf8(exception.what());
                    dialog->reject();
                }
            });
        timer.start(10);
        auto action = original.findChild<QAction*>("editExistingTextBlocks");
        check(action && action->isEnabled(), "Actual existing body menu available");
        action->trigger();
        timer.stop();
        check(error.isEmpty(), error);
    }
    check(original.doc.cursor == cursor + 2, "Two body applies are two Undo units");
    const auto edited = encodePdf(original.doc.pdf());
    original.doc.undo();
    original.doc.undo();
    check(encodePdf(original.doc.pdf()) == before, "Body UI Undo restores source and signature");
    original.doc.redo();
    original.doc.redo();
    check(encodePdf(original.doc.pdf()) == edited, "Body UI Redo restores exact edits");
    original.doc.save(output + "/existing-text-ui.pdf");
    Document reopened;
    reopened.open(output + "/existing-text-ui.pdf");
    const auto text = pageText(reopened.pdf(), 0);
    check(text.contains("Edited body line") && text.contains("本文の日本語を編集します"),
          "Saved UI body searchable");
    check(signatures(reopened.pdf(), 2).size() == 1 && formFields(reopened.pdf()).size() == 2,
          "Outside signature and forms retained");
    check(fileHash(fixtures + "/existing-text-edit/visible/body-text.pdf") ==
              fileHash(original.doc.source),
          "Original file unchanged");
    return {{"actual_menu_keyboard_Japanese_Qt_paste", true},
            {"Undo_Redo_save_reopen", true},
            {"native_IME_and_OS_clipboard", "未実行"}};
}
QJsonObject testExistingTextUiRefusals(const QString& fixtures, const QString& output)
{
    auto source = readPdf(fixtures + "/existing-text-edit/visible/body-text.pdf");
    const auto before = encodePdf(source);
    ExistingTextDialog dialog(source, 0);
    dialog.show();
    ready(dialog);
    dialog.findChild<QListWidget*>("existingTextList")
        ->setCurrentRow(row(dialog, "Original body line"));
    ready(dialog);
    auto input = dialog.findChild<QPlainTextEdit*>("existingTextInput");
    input->setPlainText("未収録漢字");
    ready(dialog);
    check(!dialog.findChild<QPushButton*>("applyExistingText")->isEnabled() &&
              dialog.findChild<QLabel*>("existingTextMessage")->text().contains("字体"),
          "Unknown glyph refuses candidate without Apply");
    input->setPlainText("Edited body line");
    ready(dialog);
    check(dialog.findChild<QPushButton*>("applyExistingText")->isEnabled(),
          "Supported text can retry");
    auto op = dialog.findChild<QComboBox*>("existingTextOperation");
    op->setCurrentIndex(2);
    ready(dialog);
    auto apply = dialog.findChild<QPushButton*>("applyExistingText");
    check(!apply->isEnabled(), "Body deletion requires explicit consent");
    auto consent = dialog.findChild<QCheckBox*>("existingTextDeleteConsent");
    consent->setFocus();
    QTest::keyClick(consent, Qt::Key_Space);
    check(apply->isEnabled(), "Keyboard consent enables concrete deletion");
    apply->click();
    writeCandidate(dialog.takeDocument(), output + "/existing-text-ui-removed.pdf");
    check(encodePdf(source) == before, "Dialog candidates preserve original snapshot");
    ExistingTextDialog cancel(source, 0);
    cancel.show();
    ready(cancel);
    cancel.findChild<QListWidget*>("existingTextList")
        ->setCurrentRow(row(cancel, "Original body line"));
    ready(cancel);
    cancel.findChild<QPlainTextEdit*>("existingTextInput")->setPlainText("Edited body line");
    check(QTest::qWaitFor(
              [&]
              {
                  for (auto thread : cancel.findChildren<QThread*>())
                      if (thread->isRunning())
                          return true;
                  return false;
              },
              15000),
          "Body worker running before Esc");
    QTest::keyClick(&cancel, Qt::Key_Escape);
    check(QTest::qWaitFor([&] { return !cancel.isVisible(); }, 15000) &&
              cancel.result() == QDialog::Rejected,
          "Running body candidate Esc cancels");
    check(encodePdf(source) == before, "Worker cancellation preserves source");
    ExistingTextDialog geometry(source, 0);
    geometry.show();
    ready(geometry);
    geometry.findChild<QListWidget*>("existingTextList")
        ->setCurrentRow(row(geometry, "Original body line"));
    ready(geometry);
    geometry.findChild<QComboBox*>("existingTextOperation")->setCurrentIndex(1);
    ready(geometry);
    auto x = geometry.findChild<QDoubleSpinBox*>("existingTextX");
    x->setFocus();
    QTest::keyClick(x, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(x, "25.4");
    geometry.findChild<QDoubleSpinBox*>("existingTextY")->setValue(50.8);
    geometry.findChild<QDoubleSpinBox*>("existingTextWidth")->setValue(76.2);
    geometry.findChild<QDoubleSpinBox*>("existingTextHeight")->setValue(10.16);
    ready(geometry);
    auto geometryApply = geometry.findChild<QPushButton*>("applyExistingText");
    check(geometryApply->isEnabled(), "Body numeric geometry ready");
    geometryApply->click();
    writeCandidate(geometry.takeDocument(), output + "/existing-text-ui-moved.pdf");
    const auto externalPath = output + "/existing-text-ui-external-source.pdf";
    writeCandidate(source, externalPath);
    const auto hash = fileHash(externalPath);
    ExistingTextDialog external(source, 0,
                                [externalPath, hash]
                                {
                                    if (fileHash(externalPath) != hash)
                                        fail("元のPDFが更新されました。");
                                });
    external.show();
    ready(external);
    external.findChild<QListWidget*>("existingTextList")
        ->setCurrentRow(row(external, "Original body line"));
    ready(external);
    external.findChild<QPlainTextEdit*>("existingTextInput")->setPlainText("Edited body line");
    ready(external);
    QFile changed(externalPath);
    check(changed.open(QIODevice::Append), "Update owned test input");
    changed.write("\n% external update\n");
    changed.close();
    external.findChild<QPushButton*>("applyExistingText")->click();
    check(external.isVisible() &&
              !external.findChild<QPushButton*>("applyExistingText")->isEnabled() &&
              external.findChild<QLabel*>("existingTextMessage")->text().contains("更新"),
          "External update blocks Apply");
    external.reject();
    return {{"unknown_glyph_no_partial_apply", true},
            {"explicit_delete_consent", true},
            {"running_Esc", true},
            {"numeric_geometry_and_source_update", true}};
}
} // namespace tatsu
