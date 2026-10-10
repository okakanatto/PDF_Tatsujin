#include "existing_image_dialog.h"
#include "existing_image_tests.h"
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
} // namespace
QJsonObject testExistingImageUi(const QString& fixtures, const QString& output)
{
    Window original;
    original.doc.open(fixtures + "/existing-image-edit/shared-images.pdf");
    original.doc.putSignature(2, "既存画像の範囲外署名", {30, 30}, 10, Qt::black);
    original.refresh(true);
    original.show();
    const auto before = encodePdf(original.doc.pdf());
    const auto cursor = original.doc.cursor;
    writeCandidate(original.doc.pdf(), output + "/existing-image-ui-source.pdf");
    QString error;
    bool handling = false;
    QTimer timer;
    QObject::connect(
        &timer, &QTimer::timeout,
        [&]
        {
            auto dialog = dynamic_cast<ExistingImageDialog*>(QApplication::activeModalWidget());
            if (!dialog || handling)
                return;
            handling = true;
            try
            {
                auto preview = dynamic_cast<PageRegionPreview*>(
                    dialog->findChild<QWidget*>("existingImagePreview"));
                auto ready = [&]
                {
                    check(QTest::qWaitFor(
                              [&] {
                                  return preview->property("renderReady").toBool() &&
                                         !preview->image.isNull();
                              },
                              15000),
                          dialog->findChild<QLabel*>("existingImageMessage")->text());
                };
                ready();
                const auto firstHeight = preview->image.height();
                preview->setZoom(2, preview->rect().center());
                ready();
                check(preview->image.height() >= firstHeight * 1.9,
                      "Zoom rerenders vector PDF instead of only stretching the preview raster");
                preview->fitPage();
                ready();
                const auto images = existingImages(original.doc.pdf(), 0);
                QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier,
                                  preview->physicalToWidget(images[1].physical.center()).toPoint());
                auto list = dialog->findChild<QListWidget*>("existingImageList");
                check(list->currentRow() == 1, "Actual page click selects second shared image");
                list->setCurrentRow(0);
                ready();
                const auto center = images[0].physical.center();
                QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier,
                                  preview->physicalToWidget(center).toPoint());
                QTest::mouseMove(preview,
                                 preview->physicalToWidget(center + QPointF(20, 15)).toPoint(), 60);
                QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier,
                                    preview->physicalToWidget(center + QPointF(20, 15)).toPoint());
                check(dialog->findChild<QDoubleSpinBox*>("existingImageX")->value() * 72 / 25.4 >
                          images[0].physical.x() + 10,
                      "Real drag updates position before numeric normalization");
                auto x = dialog->findChild<QDoubleSpinBox*>("existingImageX");
                x->setFocus();
                QTest::keyClick(x, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClicks(x, "25.4");
                QTest::keyClick(x, Qt::Key_Return);
                dialog->findChild<QDoubleSpinBox*>("existingImageY")->setValue(152.4);
                dialog->findChild<QDoubleSpinBox*>("existingImageWidth")->setValue(50.8);
                auto apply = dialog->findChild<QPushButton*>("applyExistingImage");
                check(QTest::qWaitFor(
                          [&] {
                              return dialog->findChild<QLabel*>("existingImageMessage")
                                  ->text()
                                  .contains("OCR");
                          },
                          15000) &&
                          !apply->isEnabled() && encodePdf(original.doc.pdf()) == before,
                      "Initial fixed UI proposal crosses invisible text and is refused without "
                      "mutation");
                dialog->findChild<QDoubleSpinBox*>("existingImageY")->setValue(304.8);
                check(qAbs(dialog->findChild<QDoubleSpinBox*>("existingImageHeight")->value() -
                           25.4) < .0001,
                      "Numeric resize preserves aspect ratio");
                ready();
                check(dialog->grab().save(output + "/existing-image-editing.png"),
                      "Actual candidate preview screenshot");
                dialog->resize(800, 480);
                QTest::qWait(30);
                check(dialog->grab().save(output + "/existing-image-compact.png"),
                      "Compact actual image editor screenshot");
                check(apply->isEnabled(), "Concrete candidate is ready before Apply");
                apply->click();
            }
            catch (const std::exception& exception)
            {
                error = QString::fromUtf8(exception.what());
                dialog->reject();
            }
        });
    timer.start(10);
    auto action = original.findChild<QAction*>("editExistingImages");
    check(action && action->isEnabled(), "Actual document image edit menu available");
    action->trigger();
    timer.stop();
    check(error.isEmpty(), error);
    check(original.doc.cursor == cursor + 1 && original.doc.dirty(),
          "UI applies one image edit to one Undo unit");
    const auto edited = encodePdf(original.doc.pdf());
    const auto rectangle = existingImages(original.doc.pdf(), 0)[0].physical;
    check(qAbs(rectangle.x() - 72) < 1e-7 && qAbs(rectangle.y() - 864) < 1e-7 &&
              qAbs(rectangle.width() - 144) < 1e-7 && qAbs(rectangle.height() - 72) < 1e-7,
          "UI fixed physical image geometry applied");
    original.doc.undo();
    check(encodePdf(original.doc.pdf()) == before,
          "UI Undo retains original image and outside signature");
    original.doc.redo();
    check(encodePdf(original.doc.pdf()) == edited, "UI Redo restores edited image");
    original.doc.save(output + "/existing-image-ui.pdf");
    Document reopened;
    reopened.open(output + "/existing-image-ui.pdf");
    check(signatures(reopened.pdf(), 2).size() == 1 &&
              signatures(reopened.pdf(), 2)[0].text == "既存画像の範囲外署名" &&
              formFields(reopened.pdf()).size() == 2,
          "Saved UI image, Japanese signature and forms reopen");
    for (int number = 0; number < original.doc.pages(); ++number)
        check(pageText(original.doc.pdf(), number) == pageText(reopened.pdf(), number),
              "Saved image edit keeps original searchable text");
    return {{"menu_page_selection_mouse_drag_keyboard_numeric_input", true},
            {"actual_candidate_preview_before_apply", true},
            {"single_Undo_Redo_and_saved_reopen", true},
            {"original_UI_proposal_refused_without_OCR_mutation", true},
            {"outside_Japanese_signature_and_forms_preserved", true},
            {"native_IME", "未実行"}};
}
QJsonObject testExistingImageUiModes(const QString& fixtures, const QString& output)
{
    auto snapshot = readPdf(fixtures + "/existing-image-edit/shared-images.pdf");
    const auto before = encodePdf(snapshot);
    auto ready = [](ExistingImageDialog& dialog)
    {
        auto preview = dialog.findChild<QWidget*>("existingImagePreview");
        check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
              dialog.findChild<QLabel*>("existingImageMessage")->text());
    };
    {
        ExistingImageDialog dialog(snapshot, 0, {});
        dialog.show();
        ready(dialog);
        dialog.findChild<QComboBox*>("existingImageOperation")->setCurrentIndex(1);
        auto path = dialog.findChild<QLineEdit*>("existingImageReplacement");
        path->setText(fixtures + "/existing-image-edit/replacement.png");
        path->setFocus();
        QTest::keyClick(path, Qt::Key_Return);
        ready(dialog);
        check(dialog.findChild<QPushButton*>("applyExistingImage")->isEnabled(),
              "PNG replacement preview is ready before Apply");
        check(dialog.grab().save(output + "/existing-image-replacement-preview.png"),
              "Actual replacement preview screenshot");
        dialog.findChild<QPushButton*>("applyExistingImage")->click();
        check(dialog.result() == QDialog::Accepted && !dialog.isVisible(),
              "Replacement explicitly accepted");
        writeCandidate(dialog.takeDocument(), output + "/existing-image-ui-replaced-png.pdf");
    }
    {
        ExistingImageDialog dialog(snapshot, 0, {});
        dialog.show();
        ready(dialog);
        dialog.findChild<QComboBox*>("existingImageOperation")->setCurrentIndex(2);
        ready(dialog);
        auto apply = dialog.findChild<QPushButton*>("applyExistingImage");
        check(!apply->isEnabled(), "Delete preview requires explicit consent");
        auto consent = dialog.findChild<QCheckBox*>("existingImageDeleteConsent");
        consent->setFocus();
        QTest::keyClick(consent, Qt::Key_Space);
        check(apply->isEnabled(), "Keyboard confirms deletion");
        apply->click();
        check(dialog.result() == QDialog::Accepted, "Deletion explicitly accepted");
        writeCandidate(dialog.takeDocument(), output + "/existing-image-ui-removed.pdf");
    }
    {
        ExistingImageDialog dialog(snapshot, 0, {});
        dialog.show();
        check(QTest::qWaitFor(
                  [&]
                  {
                      for (auto thread : dialog.findChildren<QThread*>())
                          if (thread->isRunning())
                              return true;
                      return false;
                  },
                  15000),
              "Real preview worker observed running before Esc");
        QTest::keyClick(&dialog, Qt::Key_Escape);
        check(QTest::qWaitFor([&] { return !dialog.isVisible(); }, 15000) &&
                  dialog.result() == QDialog::Rejected,
              "Esc closes preview worker without applying");
    }
    {
        const auto owned = output + "/existing-image-owned-input.pdf";
        check(!QFileInfo::exists(owned) &&
                  QFile::copy(fixtures + "/existing-image-edit/shared-images.pdf", owned),
              "Copy owned source for real input update test");
        const auto expected = fileHash(owned);
        ExistingImageDialog dialog(snapshot, 0,
                                   [owned, expected]
                                   {
                                       if (fileHash(owned) != expected)
                                           fail("元のPDFが更新されました。");
                                   });
        dialog.show();
        ready(dialog);
        dialog.findChild<QDoubleSpinBox*>("existingImageX")->setValue(63.5);
        ready(dialog);
        QFile file(owned);
        check(file.open(QIODevice::Append) && file.write("\n% EXTERNAL_TEST_CHANGE\n") > 0 &&
                  file.flush(),
              "Change owned input bytes after preview");
        file.close();
        auto apply = dialog.findChild<QPushButton*>("applyExistingImage");
        check(apply->isEnabled(), "Candidate ready before input update validation");
        apply->click();
        check(dialog.isVisible() && !apply->isEnabled() &&
                  dialog.findChild<QLabel*>("existingImageMessage")->text().contains("更新"),
              "Updated source refuses Apply without publication");
        QTest::keyClick(&dialog, Qt::Key_Escape);
        check(!dialog.isVisible(), "Close refused edit");
    }
    check(encodePdf(snapshot) == before,
          "Replacement, deletion, cancellation and refusal preserve source snapshot");
    return {{"PNG_replacement_preview_and_apply", true},
            {"delete_preview_and_keyboard_consent", true},
            {"running_preview_observed_before_Esc", true},
            {"actual_input_update_refuses_apply", true},
            {"source_snapshot_preserved", true}};
}
} // namespace tatsu
