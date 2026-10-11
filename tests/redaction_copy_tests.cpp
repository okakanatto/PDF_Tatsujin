#include "redaction_copy_tests.h"
#include "form_fields.h"
#include "page_operations.h"
#include "redaction_dialog.h"
#include "redaction_ui_test_support.h"
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
QString foundation(const QString& fixtures)
{
    return fixtures + "/redaction-copy/foundation";
}
QJsonObject json(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read frozen redaction criteria");
    return QJsonDocument::fromJson(file.readAll()).object();
}
QMap<int, QVector<QRectF>> plan(const QString& fixtures)
{
    QFile file(foundation(fixtures) + "/plan.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen plan");
    QMap<int, QVector<QRectF>> result;
    for (const auto& value : QJsonDocument::fromJson(file.readAll()).array())
    {
        const auto entry = value.toObject();
        const auto rect = entry["rect"].toArray();
        check(rect.size() == 4, "Frozen rectangle");
        result[entry["page"].toInt()] << QRectF(rect[0].toDouble(), rect[1].toDouble(),
                                                rect[2].toDouble(), rect[3].toDouble());
    }
    return result;
}
PDFDocument source(const QString& fixtures)
{
    const auto path = foundation(fixtures) + "/unsafe-source.pdf";
    check(fileHash(path).toHex() ==
              json(foundation(fixtures) + "/criteria.json")["source_sha256"].toString().toLatin1(),
          "Frozen foundation hash");
    return readPdf(path);
}
void writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              file.write(bytes) == bytes.size() && file.flush(),
          "Write owned atomic-test bytes");
}
using testing::add;
using testing::ready;
} // namespace
QJsonObject testRedactionCopyFoundation(const QString& fixtures, const QString& output)
{
    const auto document = source(fixtures);
    const auto before = encodePdf(document);
    const auto saved =
        exportRedactedPdf(document, plan(fixtures), output + "/redaction-foundation.pdf");
    auto reopened = readPdf(output + "/redaction-foundation.pdf");
    check(!pageText(reopened, 0).contains("SECRET_TEXT_9df67") &&
              !pageText(reopened, 0).contains("SECRET_HIDDEN_9df67") &&
              pageText(reopened, 0).contains("KEEP_VISIBLE_9df67") &&
              pageText(reopened, 1).contains("KEEP_SECOND_PAGE_9df67"),
          "Removed target and retained searchable text");
    check(encodePdf(document) == before, "Export leaves source snapshot unchanged");
    Document editable;
    editable.open(output + "/redaction-foundation.pdf");
    const auto fields = formFields(editable.pdf());
    check(fields.size() == 1 && fields.first().qualifiedName == "keep-field",
          "Remaining editable form");
    putFormValue(editable, fields.first().widget, {"EDIT_AFTER_REDACTION"});
    editable.save(output + "/redaction-form-reedited.pdf");
    QJsonArray fonts;
    const auto directory = fixtures + "/redaction-copy/fonts";
    for (const auto& value : json(directory + "/criteria.json")["cases"].toArray())
    {
        const auto entry = value.toObject();
        if (entry["expect_rejection"].toBool())
            continue;
        const auto name = entry["file"].toString();
        const auto path = directory + "/" + name;
        check(fileHash(path).toHex() == entry["sha256"].toString().toLatin1(),
              "Frozen Japanese font hash");
        auto input = readPdf(path);
        const QRectF rectangle = name == "following-font-switch.pdf" ? QRectF(48, 620, 145, 65)
                                                                     : QRectF(48, 620, 420, 65);
        const auto destination = output + "/redaction-font-" + name;
        exportRedactedPdf(input, {{0, {rectangle}}}, destination);
        auto after = readPdf(destination);
        check(!pageText(after, 0).contains(entry["remove"].toString()) &&
                  pageText(after, 0).contains(entry["keep"].toString()),
              "Japanese visible/invisible font export");
        fonts << QJsonObject{{"input", name},
                             {"output", QFileInfo(destination).fileName()},
                             {"sha256", QString(fileHash(destination).toHex())}};
    }
    return {{"source_unchanged", true},
            {"remaining_form_reinput_and_save", true},
            {"removed_text_segments", saved.textSegments},
            {"modified_images", saved.images},
            {"removed_field_groups", saved.fieldGroups},
            {"font_outputs", fonts}};
}
QJsonObject testRedactionCopyAtomic(const QString& fixtures, const QString& output)
{
    const auto document = source(fixtures);
    const auto before = encodePdf(document);
    const auto regions = plan(fixtures);
    QJsonArray rejected;
    auto reject = [&](QString name, QString destination, std::function<bool()> cancel = {},
                      std::function<void(QString)> progress = {},
                      std::function<void()> validate = {})
    {
        bool failed = false;
        try
        {
            exportRedactedPdf(document, regions, destination, cancel, progress, validate);
        }
        catch (const std::exception&)
        {
            failed = true;
        }
        check(failed && encodePdf(document) == before,
              "Rejected export preserves source snapshot: " + name);
        rejected << name;
    };
    const auto existing = output + "/redaction-existing.pdf";
    writeBytes(existing, "EXISTING_FILE");
    reject("existing output", existing);
    check(fileHash(existing) ==
              QCryptographicHash::hash("EXISTING_FILE", QCryptographicHash::Sha256),
          "Existing output bytes unchanged");
    const auto racing = output + "/redaction-racing.pdf";
    reject("racing destination", racing, {},
           [&](QString status)
           {
               if (status.startsWith("墨消ししたコピーを新しい"))
                   writeBytes(racing, "RACING_FILE");
           });
    check(fileHash(racing) == QCryptographicHash::hash("RACING_FILE", QCryptographicHash::Sha256),
          "Racing output not replaced");
    for (const auto& stage : QStringList{"before", "after-write", "before-publication"})
    {
        bool cancelled = stage == "before";
        const auto destination = output + "/redaction-cancel-" + stage + ".pdf";
        reject(
            "cancel " + stage, destination, [&] { return cancelled; },
            [&](QString status)
            {
                if ((stage == "after-write" && status.startsWith("保存候補")) ||
                    (stage == "before-publication" &&
                     status.startsWith("墨消ししたコピーを新しい")))
                    cancelled = true;
            });
        check(!QFileInfo::exists(destination), "Cancelled output not published");
    }
    const auto changed = output + "/redaction-owned-input.pdf";
    writeBytes(changed, before);
    const auto hash = fileHash(changed);
    const auto after = output + "/redaction-changed-after.pdf";
    reject(
        "input changed during export", after, {},
        [&](QString status)
        {
            if (status.startsWith("墨消ししたコピーを新しい"))
                writeBytes(changed, before + "\nOWNED_INPUT_CHANGED");
        },
        [&] { check(fileHash(changed) == hash, "Original input changed"); });
    check(!QFileInfo::exists(after), "Changed input not published");
    const auto pre = output + "/redaction-changed-before.pdf";
    reject("input changed before export", pre, {}, {},
           [&] { check(fileHash(changed) == hash, "Original input already changed"); });
    check(!QFileInfo::exists(pre), "Prechanged input not published");
    reject("wrong extension", output + "/redaction-invalid.txt");
    reject("missing parent", output + "/not-created/redaction.pdf");
    QString ntfs = "未実行";
    const auto denied = qEnvironmentVariable("TATSU_DENIED_SAVE_DIR");
    if (!denied.isEmpty())
    {
        const auto existingHash = fileHash(denied + "/existing.pdf");
        const auto deniedCopy = denied + "/redaction-denied-copy.pdf";
        reject("actual NTFS write denial", deniedCopy);
        check(!QFileInfo::exists(deniedCopy) && fileHash(denied + "/existing.pdf") == existingHash,
              "NTFS denial preserves existing file and publishes no copy");
        ntfs = "PASS";
    }
    check(QDir(output)
              .entryList({".pdf-tatsujin-save-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
              .isEmpty(),
          "Owned candidates cleaned");
    return {{"rejected", rejected},
            {"actual_NTFS_write_denial", ntfs},
            {"existing_and_racing_bytes_preserved", true},
            {"input_updated_with_actual_file_write", true},
            {"source_unchanged", true}};
}
QJsonObject testRedactionCopyUi(const QString& fixtures, const QString& output)
{
    Window original;
    auto action = original.findChild<QAction*>("exportRedactedCopy");
    check(action && !action->isEnabled(), "Redaction requires editable document");
    original.openFile(foundation(fixtures) + "/unsafe-source.pdf");
    original.doc.putSignature(1, "範囲外の署名を保持", {120, 160}, 12, Qt::black);
    original.refresh();
    original.show();
    const auto before = encodePdf(original.doc.pdf());
    writeCandidate(original.doc.pdf(), output + "/redaction-ui-source-snapshot.pdf");
    const auto hash = original.doc.sourceHash;
    const auto cursor = original.doc.cursor, saved = original.doc.saved;
    QString error;
    bool invoked = false;
    QTimer timer;
    timer.setInterval(5);
    QObject::connect(
        &timer, &QTimer::timeout, &original,
        [&]
        {
            auto dialog =
                dynamic_cast<RedactionDialog*>(original.findChild<QDialog*>("redactionDialog"));
            if (!dialog || invoked)
                return;
            auto preview =
                dynamic_cast<PageRegionPreview*>(dialog->findChild<QWidget*>("redactionPreview"));
            if (preview->image.isNull())
                return;
            invoked = true;
            timer.stop();
            try
            {
                const auto ranges = plan(fixtures).value(0);
                for (const auto& rectangle : ranges)
                    add(*dialog,
                        pageMatrix(original.doc.pdf().getCatalog()->getPage(0)).mapRect(rectangle));
                auto consent = dialog->findChild<QCheckBox*>("redactionConsent");
                auto save = dialog->findChild<QPushButton*>("saveRedactedCopy");
                check(!save->isEnabled(), "No implicit approval of destructive copy");
                dialog->findChild<QLineEdit*>("redactionDestination")
                    ->setText(output + "/redaction-ui.pdf");
                consent->setChecked(true);
                auto x = dialog->findChild<QDoubleSpinBox*>("redactionX");
                const auto value = x->value();
                x->setValue(value + 1);
                check(!consent->isChecked() && !save->isEnabled(),
                      "Geometry change invalidates confirmation");
                x->setValue(value);
                consent->setChecked(true);
                dialog->findChild<QLineEdit*>("redactionDestination")
                    ->setText(output + "/redaction-ui-2.pdf");
                check(!consent->isChecked(), "Destination change invalidates confirmation");
                dialog->findChild<QLineEdit*>("redactionDestination")
                    ->setText(output + "/redaction-ui.pdf");
                dialog->resize(800, 480);
                auto scroll = dialog->findChild<QScrollArea*>();
                check(scroll && scroll->verticalScrollBar()->maximum() > 0,
                      "Compact settings scroll");
                scroll->ensureWidgetVisible(dialog->findChild<QLineEdit*>("redactionDestination"));
                consent->setFocus();
                QTest::keyClick(consent, Qt::Key_Space);
                check(consent->isChecked() && save->isEnabled(), "Keyboard explicit confirmation");
                check(dialog->grab().save(output + "/redaction-confirmed-compact.png"),
                      "Actual compact redaction screenshot");
                save->click();
            }
            catch (const std::exception& exception)
            {
                error = QString::fromUtf8(exception.what());
                dialog->reject();
            }
        });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &original,
                     [&]
                     {
                         error = "Redaction menu timed out";
                         if (auto dialog = original.findChild<QDialog*>("redactionDialog"))
                             dialog->reject();
                     });
    timer.start();
    watchdog.start(30000);
    action->trigger();
    timer.stop();
    watchdog.stop();
    check(error.isEmpty(), error);
    Window* created = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "redactedCreatedDocument")
            created = dynamic_cast<Window*>(widget);
    std::unique_ptr<Window> owned(created);
    check(invoked && created && created->doc.history.size() == 1 && !created->doc.dirty(),
          "Separate copy has no original Undo history");
    check(!pageText(created->doc.pdf(), 0).contains("SECRET_TEXT_9df67") &&
              pageText(created->doc.pdf(), 0).contains("KEEP_VISIBLE_9df67"),
          "Actual UI copy searchable outside target");
    check(signatures(created->doc.pdf(), 1).size() == 1 &&
              signatures(created->doc.pdf(), 1).first().text == "範囲外の署名を保持",
          "Outside signature preserved in actual menu export");
    check(encodePdf(original.doc.pdf()) == before && original.doc.cursor == cursor &&
              original.doc.saved == saved && original.doc.sourceHash == hash &&
              original.doc.dirty(),
          "Original unsaved state and history retained");
    original.doc.undo();
    check(signatures(original.doc.pdf(), 1).isEmpty(), "Original Undo still works");
    original.doc.redo();
    check(encodePdf(original.doc.pdf()) == before, "Original Redo still works");
    const auto fields = formFields(created->doc.pdf());
    check(fields.size() == 1, "Actual copy keeps outside field");
    putFormValue(created->doc, fields.first().widget, {"UI_EDIT_AFTER_REDACTION"});
    created->doc.save(output + "/redaction-ui-reedited.pdf");
    original.doc.saved = original.doc.cursor;
    return {{"actual_menu_draw_confirm_export_and_separate_window", true},
            {"original_Undo_Redo", true},
            {"remaining_form_reedited", true},
            {"compact_scroll_keyboard_confirmation", true},
            {"native_IME_OS_clipboard_DPI", "未実行"}};
}
QJsonObject testRedactionCopyFontUi(const QString& fixtures, const QString& output)
{
    const auto directory = fixtures + "/redaction-copy/fonts";
    for (const auto& name : QStringList{"visible", "invisible-OCR", "surviving-subset-text"})
    {
        const auto document = readPdf(directory + "/" + name + ".pdf");
        const auto path = output + "/redaction-ui-font-" + name + ".pdf";
        RedactionDialog dialog(document, 0, {}, path);
        dialog.show();
        add(dialog,
            pageMatrix(document.getCatalog()->getPage(0)).mapRect(QRectF(48, 620, 420, 65)));
        dialog.findChild<QCheckBox*>("redactionConsent")->setChecked(true);
        dialog.findChild<QPushButton*>("saveRedactedCopy")->click();
        if (name == "surviving-subset-text")
        {
            auto progress = dialog.findChild<QProgressBar*>("redactionProgress");
            check(QTest::qWaitFor([&] { return !progress->isVisible(); }, 30000),
                  "Unsupported-font worker completes");
            check(dialog.savedPath().isEmpty() && !QFileInfo::exists(path) &&
                      dialog.findChild<QLabel*>("redactionMessage")
                          ->text()
                          .contains("サブセット書体"),
                  "Actual UI refuses shared outside glyphs without copy");
            QTest::keyClick(&dialog, Qt::Key_Escape);
        }
        else
            check(QTest::qWaitFor([&] { return !dialog.isVisible(); }, 30000) &&
                      dialog.savedHash() == fileHash(path),
                  "Actual Japanese visible/invisible UI output");
    }
    RedactionDialog cancelled(source(fixtures), 0, {}, output + "/redaction-ui-cancelled.pdf");
    cancelled.show();
    add(cancelled,
        pageMatrix(source(fixtures).getCatalog()->getPage(0)).mapRect(QRectF(48, 620, 420, 65)));
    cancelled.findChild<QCheckBox*>("redactionConsent")->setChecked(true);
    cancelled.findChild<QPushButton*>("saveRedactedCopy")->click();
    QTest::keyClick(&cancelled, Qt::Key_Escape);
    check(QTest::qWaitFor([&] { return !cancelled.isVisible(); }, 30000) &&
              !QFileInfo::exists(output + "/redaction-ui-cancelled.pdf"),
          "Running UI Escape cancels export");
    return {{"visible_invisible_Japanese_exports", true},
            {"shared_font_rejected_in_UI", true},
            {"running_Escape_without_output", true}};
}
QJsonObject testRedactionCopyGeometryUi(const QString& fixtures, const QString& output)
{
    const auto document = source(fixtures);
    RedactionDialog dialog(document, 1, {}, output + "/redaction-geometry-ui.pdf");
    dialog.show();
    auto preview = ready(dialog, 1);
    const auto originalPaper = preview->paper();
    dialog.findChild<QPushButton*>("redactionZoomIn")->click();
    check(preview->paper().width() > originalPaper.width(), "Actual preview zoom button");
    QWheelEvent wheel(preview->rect().center(), preview->mapToGlobal(preview->rect().center()),
                      QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                      Qt::NoScrollPhase, false);
    const auto zoom = preview->zoom();
    QApplication::sendEvent(preview, &wheel);
    check(preview->zoom() > zoom, "Actual Ctrl wheel zoom");
    const auto beforePan = preview->paper();
    const auto center = preview->rect().center();
    QTest::mousePress(preview, Qt::MiddleButton, Qt::NoModifier, center);
    QTest::mouseMove(preview, center + QPoint(25, 18));
    QTest::mouseRelease(preview, Qt::MiddleButton, Qt::NoModifier, center + QPoint(25, 18));
    check(preview->paper().topLeft() != beforePan.topLeft(), "Middle-button preview pan");
    const QRectF target(preview->physical.width() / 2 - 15, preview->physical.height() / 2 - 15, 30,
                        30);
    dialog.findChild<QPushButton*>("redactionDraw")->click();
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier,
                      preview->physicalToWidget(target.topLeft()).toPoint());
    QTest::mouseMove(preview, preview->physicalToWidget(target.bottomRight()).toPoint());
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier,
                        preview->physicalToWidget(target.bottomRight()).toPoint());
    check(preview->regions.size() == 1, "Draw while zoomed and panned");
    const auto drawn = preview->regions.first().second;
    const double scale = preview->paper().width() / preview->physical.width();
    for (double delta : {drawn.left() - target.left(), drawn.top() - target.top(),
                         drawn.right() - target.right(), drawn.bottom() - target.bottom()})
        check(std::abs(delta) * scale <= 1, "Zoomed gesture geometry within one input pixel");
    dialog.findChild<QPushButton*>("redactionDelete")->click();
    dialog.findChild<QPushButton*>("redactionFit")->click();
    check(preview->zoom() == 1 && preview->paper() == originalPaper,
          "Fit restores original page geometry");
    const auto rectangle =
        pageMatrix(document.getCatalog()->getPage(1)).mapRect(QRectF(48, 620, 420, 65));
    add(dialog, rectangle, 1);
    auto consent = dialog.findChild<QCheckBox*>("redactionConsent");
    consent->setChecked(true);
    const auto region = preview->regions.first().second;
    auto start = preview->physicalToWidget(region.center()).toPoint();
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(preview, start + QPoint(10, 8));
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, start + QPoint(10, 8));
    check(!consent->isChecked() && preview->regions.first().second != region,
          "Actual region drag invalidates confirmation");
    auto box = preview->regions.first().second;
    start = preview->physicalToWidget(box.bottomRight()).toPoint();
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(preview, start + QPoint(5, 5));
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, start + QPoint(5, 5));
    check(preview->regions.first().second.size() != box.size(), "Actual corner resize");
    constexpr double mm = 72.0 / 25.4;
    for (const auto& value :
         QVector<QPair<QString, double>>{{"redactionX", rectangle.x() / mm},
                                         {"redactionY", rectangle.y() / mm},
                                         {"redactionWidth", rectangle.width() / mm},
                                         {"redactionHeight", rectangle.height() / mm}})
        dialog.findChild<QDoubleSpinBox*>(value.first)->setValue(value.second);
    dialog.findChild<QComboBox*>("redactionPage")->setCurrentIndex(0);
    ready(dialog, 0);
    check(!dialog.findChild<QDoubleSpinBox*>("redactionX")->isEnabled(),
          "Page change does not edit an invisible selection");
    add(dialog, QRectF(15, 15, 30, 30));
    auto list = dialog.findChild<QListWidget*>("redactionRegions");
    check(list->count() == 2, "Temporary second-page range");
    auto field = dialog.findChild<QDoubleSpinBox*>("redactionWidth");
    field->setFocus();
    QTest::keyClick(field, Qt::Key_Delete);
    check(list->count() == 2, "Delete in numeric text does not remove a range");
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Delete);
    check(list->count() == 1, "Delete acts on range list only");
    list->setCurrentRow(0);
    ready(dialog, 1);
    consent->setChecked(true);
    check(dialog.grab().save(output + "/redaction-rotated-page.png"),
          "Actual rotated-page screenshot");
    dialog.findChild<QPushButton*>("saveRedactedCopy")->click();
    check(QTest::qWaitFor([&] { return !dialog.isVisible(); }, 30000),
          "Actual rotated-page copy completes");
    auto after = readPdf(dialog.savedPath());
    check(!pageText(after, 1).contains("KEEP_SECOND_PAGE_9df67") &&
              pageText(after, 0).contains("SECRET_TEXT_9df67"),
          "Selected rotated text removed; unselected page retained");
    check(formFields(after).size() == 2, "Unselected page forms retained");
    return {{"raw_target", QJsonArray{48, 620, 420, 65}},
            {"page", 1},
            {"rotation_CropBox_UserUnit", true},
            {"zoom_pan_fit", true},
            {"drag_resize_confirmation_invalidation", true},
            {"Delete_focus_safety", true},
            {"unselected_page_retained", true}};
}
QJsonObject testRedactionCopyOcr(const QString& fixtures, const QString& output)
{
    Window original;
    original.doc.history = {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6})};
    original.doc.saved = -1;
    original.doc.putSignature(1, "墨消し範囲外の日本語署名", {30, 30}, 10, Qt::black);
    original.refresh(true);
    original.show();
    QString error;
    QTimer messages;
    QObject::connect(&messages, &QTimer::timeout,
                     [&]
                     {
                         if (auto box =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (box->windowTitle() != "OCR結果")
                                 error = box->text();
                             box->accept();
                         }
                     });
    messages.start(10);
    original.language->setCurrentIndex(0);
    original.scope->setCurrentIndex(0);
    original.startOcr();
    check(QTest::qWaitFor([&] { return !original.doc.busy && !original.worker; }, 90000),
          "Actual bilingual OCR worker completes");
    check(error.isEmpty(), error);
    messages.stop();
    check(pageText(original.doc.pdf(), 0).contains("市民公園") &&
              pageText(original.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Frozen Japanese and English OCR terms before redaction");
    const auto before = encodePdf(original.doc.pdf());
    const auto cursor = original.doc.cursor, saved = original.doc.saved;
    writeCandidate(original.doc.pdf(), output + "/redaction-real-ocr-before.pdf");
    bool partialRejected = false;
    try
    {
        exportRedactedPdf(original.doc.pdf(), {{0, {QRectF(48, 620, 420, 65)}}},
                          output + "/redaction-partial-ocr-refused.pdf");
    }
    catch (const std::exception& exception)
    {
        partialRejected = QString::fromUtf8(exception.what()).contains("一部だけ");
    }
    check(partialRejected && !QFileInfo::exists(output + "/redaction-partial-ocr-refused.pdf") &&
              encodePdf(original.doc.pdf()) == before,
          "Partial actual OCR layer rejected without output or source change");
    const auto path = output + "/redaction-real-ocr-ui.pdf";
    RedactionDialog dialog(original.doc.pdf(), 0, {}, path);
    dialog.show();
    ready(dialog, 0);
    dialog.findChild<QPushButton*>("redactionWholePage")->click();
    dialog.findChild<QCheckBox*>("redactionConsent")->setChecked(true);
    dialog.findChild<QPushButton*>("saveRedactedCopy")->click();
    auto progress = dialog.findChild<QProgressBar*>("redactionProgress");
    check(QTest::qWaitFor([&] { return !progress->isVisible(); }, 30000),
          "OCR redaction worker completes");
    check(!dialog.savedPath().isEmpty() && !dialog.isVisible(),
          dialog.findChild<QLabel*>("redactionMessage")->text());
    Window copy;
    copy.doc.open(path);
    copy.refresh(true);
    copy.show();
    check(pageText(copy.doc.pdf(), 0).trimmed().isEmpty() &&
              pageText(copy.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Selected Japanese OCR removed; English OCR retained");
    check(signatures(copy.doc.pdf(), 1).size() == 1 &&
              signatures(copy.doc.pdf(), 1).first().text == "墨消し範囲外の日本語署名",
          "Outside Japanese signature retained after actual OCR redaction");
    check(encodePdf(original.doc.pdf()) == before && original.doc.cursor == cursor &&
              original.doc.saved == saved && original.doc.dirty(),
          "Original OCR and unsaved history preserved");
    original.doc.undo();
    check(!pageText(original.doc.pdf(), 0).contains("市民公園"), "Original OCR Undo still works");
    original.doc.redo();
    check(encodePdf(original.doc.pdf()) == before, "Original OCR Redo still works");
    auto search = copy.findChild<SearchPanel*>("searchPanel");
    copy.canvas->setFocus();
    QTest::keyClick(copy.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    copy.query->setText("coastal");
    QTest::keyClick(copy.query, Qt::Key_Return);
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              15000),
          "Actual search in saved redacted OCR copy");
    copy.canvas->setZoom(.5);
    copy.canvas->goToPage(1);
    check(QTest::qWaitFor([&] { return copy.canvas->pageReady(1); }, 15000),
          "Saved English OCR page ready");
    const auto crop = copy.doc.pdf().getCatalog()->getPage(1)->getCropBox();
    const auto first =
        copy.canvas->pdfToViewport(1, crop.topLeft() + QPointF(1, crop.height() - 1)).toPoint();
    const auto last =
        copy.canvas->pdfToViewport(1, crop.topLeft() + QPointF(crop.width() - 1, 1)).toPoint();
    QTest::mousePress(copy.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(copy.canvas->viewport(), last, 60);
    QTest::mouseRelease(copy.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return copy.canvas->selectionReady(); }, 15000),
          "Saved OCR selection ready");
    QTest::keyClick(copy.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto text = QApplication::clipboard()->text();
    check(text.size() > 500 && text.contains("coastal", Qt::CaseInsensitive),
          "Actual Qt copy in saved redacted OCR PDF");
    original.doc.saved = original.doc.cursor;
    return {{"actual_bilingual_OCR_then_selected_page_redaction", true},
            {"partial_OCR_layer_rejected_without_output", true},
            {"outside_English_search_and_Qt_copy", true},
            {"copied_characters", text.size()},
            {"original_OCR_Undo_Redo", true},
            {"native_clipboard", "未実行"}};
}
} // namespace tatsu
