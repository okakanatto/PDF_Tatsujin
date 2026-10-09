#include "encryption_tests.h"
#include "bookmark_edit.h"
#include "encryption_dialog.h"
#include "form_fields.h"
#include "link_edit.h"
#include "page_decoration.h"
#include "page_operations.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "pdfsecurityhandler.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& text)
{
    if (!value)
        fail(text);
}
template <typename F> QString rejects(F operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
    fail("Invalid encryption operation succeeded");
}
EncryptionOptions passwords()
{
    return {"閲覧-User-2026", "変更-Owner-2026"};
}
PDFDocument metadata(const PDFDocument& document, bool pdfA = false)
{
    PDFDocumentBuilder builder(&document);
    PDFDictionary dictionary;
    detail::set(dictionary, "Type", PDFObject::createName("Metadata"));
    detail::set(dictionary, "Subtype", PDFObject::createName("XML"));
    auto xml = QByteArray(
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'><secret>ENCRYPTION-METADATA-SECRET-2026</secret>");
    if (pdfA)
        xml += "<id:part xmlns:id='http://www.aiim.org/pdfa/ns/id/'>2</id:part>";
    xml += "</x:xmpmeta>";
    const auto ref = builder.addObject(detail::streamObject(dictionary, xml));
    auto catalog = *builder.getObjectByReference(builder.getCatalogReference()).getDictionary();
    detail::set(catalog, "Metadata", PDFObject::createReference(ref));
    builder.setObject(builder.getCatalogReference(), detail::dictObject(catalog));
    return builder.build();
}
void roundTrip(PDFDocument before, const QString& destination, const EncryptionOptions& options)
{
    exportProtectedPdf(before, options, destination);
    int role = 0;
    for (auto password : {options.userPassword, options.ownerPassword})
    {
        auto reopened = readPdf(destination, password);
        check(reopened.getStorage().getSecurityHandler()->getAuthorizationResult() ==
                  (role++ == 0 ? PDFSecurityHandler::AuthorizationResult::UserAuthorized
                               : PDFSecurityHandler::AuthorizationResult::OwnerAuthorized),
              "Password roles preserved");
        check(reopened.getIdPart(0).size() >= 16, "Encrypted output has document ID");
        for (int page = 0; page < int(before.getCatalog()->getPageCount()); ++page)
            check(renderPage(before, page, .6) == renderPage(reopened, page, .6) &&
                      pageText(before, page) == pageText(reopened, page),
                  "Pixels and text identical after password authentication");
    }
    rejects([&] { readPdf(destination); });
    rejects([&] { readPdf(destination, "wrong-password"); });
    QFile file(destination);
    check(file.open(QIODevice::ReadOnly), "Read encrypted bytes");
    const auto data = file.readAll();
    check(!data.contains("ENCRYPTION-METADATA-SECRET-2026") &&
              !data.contains(options.userPassword.toUtf8()) &&
              !data.contains(options.ownerPassword.toUtf8()),
          "Metadata and passwords absent from clear file bytes");
}
} // namespace
QJsonObject testEncryptionPasswords(const QString& output)
{
    QJsonArray rows;
    const QList<QPair<QString, QString>> valid = {
        {"I\u00adX", "IX"},
        {"a\u00a0b", "a b"},
        {"\u2168", "IX"},
        {"\u00aa", "a"},
        {"Case-ABC", "Case-ABC"},
        {"閲覧-User-2026", "閲覧-User-2026"},
        {"\u0627\u0628", "\u0627\u0628"},
        {QString(127, 'x'), QString(127, 'x')},
        {QString("日").repeated(42) + "x", QString("日").repeated(42) + "x"}};
    for (const auto& [input, expected] : valid)
    {
        const auto actual = preparePdfPassword(input);
        check(actual == expected, "Fixed SASLprep vector");
        rows << QJsonObject{{"input", input}, {"prepared", actual}, {"status", "PASS"}};
    }
    const QStringList invalid = {"",
                                 "\u0007",
                                 "\ue000",
                                 "\u0221",
                                 "\U0001f600",
                                 "\u0627a\u0628",
                                 "1\u0627",
                                 QString(128, 'x'),
                                 QString("日").repeated(43),
                                 QString(QChar(0xd800))};
    for (const auto& input : invalid)
        rows << QJsonObject{{"input", input},
                            {"error", rejects([&] { preparePdfPassword(input); })},
                            {"status", "REJECTED"}};
    QFile file(output + "/encryption-password-vectors.json");
    check(file.open(QIODevice::WriteOnly), "Write synthetic password vectors");
    file.write(QJsonDocument(rows).toJson());
    return {{"accepted_vectors", valid.size()},
            {"rejected_vectors", invalid.size()},
            {"silent_truncation", false}};
}
QJsonObject testEncryptionLifecycle(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    const auto sourceHash = fileHash(document.source);
    document.putSignature(0, "暗号化しても保持", {100, 250}, 12, Qt::black);
    auto links = editableLinks(document.pdf());
    links << LinkEntry{0, -1, {20, 20, 120, 35}, "別ページへ", LinkTarget::Page, 1, {}};
    document.commit(replaceLinks(document.pdf(), links));
    document.commit(replaceBookmarks(document.pdf(), {{{}, "日本語のしおり", -1, 2, true, true}}));
    DecorationOptions decoration;
    decoration.kind = DecorationKind::Watermark;
    decoration.watermark = "保護";
    decoration.fontFamily = signatureFont();
    document.commit(putDecoration(document.pdf(), {0, 1}, decoration));
    QImage image(80, 50, QImage::Format_RGB32);
    image.fill(QColor("#65a3ed"));
    document.putImage(0, OverlayKind::Image, image, {100, 350}, 70);
    document.commit(metadata(document.pdf()));
    // Force missing ID: the copy must create one without changing this snapshot.
    auto storage = document.pdf().getStorage();
    auto trailer = *document.pdf().getTrailerDictionary();
    detail::set(trailer, "ID", PDFObject{});
    storage.setTrailerDictionary(detail::dictObject(trailer));
    document.commit(PDFDocumentBuilder(storage, document.pdf().getInfo()->version).build());
    const auto original = document.pdf();
    const auto bytes = encodePdf(original);
    const int cursor = document.cursor, saved = document.saved;
    const auto revision = document.revision;
    writeCandidate(original, output + "/encryption-rich-before.pdf");
    roundTrip(original, output + "/encryption-rich-after.pdf", passwords());
    roundTrip(original, output + "/encryption-rich-repeat.pdf", passwords());
    check(fileHash(output + "/encryption-rich-after.pdf") !=
              fileHash(output + "/encryption-rich-repeat.pdf"),
          "Independent encryption keys and IVs produce different bytes");
    check(encodePdf(document.pdf()) == bytes && document.cursor == cursor &&
              document.saved == saved && document.revision == revision && document.dirty() &&
              document.target.isEmpty() && fileHash(document.source) == sourceHash,
          "Copy keeps source, target, history, revision and unsaved state");
    Document restricted;
    restricted.open(output + "/encryption-rich-after.pdf", passwords().userPassword);
    check(!restricted.readOnly.isEmpty() && restricted.copyAllowed,
          "Authenticated encrypted copy remains read-only");
    rejects([&] { restricted.putSignature(0, "no", {1, 1}, 12, Qt::black); });
    rejects([&] { restricted.save(output + "/implicit-decryption.pdf"); });
    document.undo();
    document.redo();
    check(document.pdf() == original, "Undo/Redo unchanged by export");
    Document form;
    form.open(fixtures + "/D07.pdf");
    for (const auto& field : formFields(form.pdf()))
        if (field.name == "name")
            putFormValue(form, field.widget, {"髙橋 香織"});
    auto formPdf = metadata(form.pdf());
    writeCandidate(formPdf, output + "/encryption-form-before.pdf");
    roundTrip(formPdf, output + "/encryption-form-after.pdf", passwords());
    auto denied = passwords();
    denied.print = denied.copy = denied.forms = false;
    roundTrip(original, output + "/encryption-denied.pdf", denied);
    auto allowed = passwords();
    allowed.annotations = allowed.assemble = allowed.modify = true;
    roundTrip(original, output + "/encryption-allowed.pdf", allowed);
    auto ascii = EncryptionOptions{"user-ASCII-2026", "owner-ASCII-2026"};
    roundTrip(original, output + "/encryption-ascii.pdf", ascii);
    return {{"user_and_owner_authentication", true},
            {"input_and_Undo_unchanged", true},
            {"randomized_ciphertext", true},
            {"pixel_difference", 0},
            {"form_links_bookmarks_overlays", true}};
}
QJsonObject testEncryptionFailures(const QString& fixtures, const QString& output)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    const auto bytes = encodePdf(source);
    QJsonArray rows;
    auto invalid = [&](QString name, const std::function<void()>& operation)
    {
        rows << QJsonObject{{"case", name}, {"error", rejects(operation)}};
        check(encodePdf(source) == bytes, "Rejected export keeps source");
    };
    invalid("empty document", [&] { protectPdf(PDFDocument{}, passwords()); });
    invalid("signed input",
            [&] { protectPdf(readPdf(fixtures + "/D08-signed.pdf"), passwords()); });
    invalid("encrypted input",
            [&]
            {
                protectPdf(
                    readPdf(output + "/encryption-rich-after.pdf", passwords().ownerPassword),
                    passwords());
            });
    invalid("PDF/A metadata", [&] { protectPdf(metadata(source, true), passwords()); });
    auto identical = EncryptionOptions{"\u2168", "IX"};
    invalid("normalized passwords equal", [&] { protectPdf(source, identical); });
    const auto existing = output + "/encryption-existing.pdf";
    writeCandidate(source, existing);
    const auto originalHash = fileHash(existing);
    invalid("existing target", [&] { exportProtectedPdf(source, passwords(), existing); });
    check(fileHash(existing) == originalHash, "Existing target unchanged");
    bool cancelled = false;
    const auto stopped = output + "/encryption-cancelled.pdf";
    invalid("cancel before publication",
            [&]
            {
                exportProtectedPdf(
                    source, passwords(), stopped, [&] { return cancelled; },
                    [&](QString text)
                    {
                        if (text.startsWith("コピー"))
                            cancelled = true;
                    });
            });
    check(cancelled && !QFileInfo::exists(stopped),
          "Actual encrypted and verified candidate discarded on cancel");
    const auto race = output + "/encryption-collision.pdf";
    invalid("destination appeared before publication",
            [&]
            {
                exportProtectedPdf(source, passwords(), race, {},
                                   [&](QString text)
                                   {
                                       if (text.startsWith("コピー"))
                                       {
                                           QFile file(race);
                                           check(
                                               file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                                               "Create actual collision");
                                           file.write("KEEP-EXISTING-COLLISION");
                                       }
                                   });
            });
    QFile collision(race);
    collision.open(QIODevice::ReadOnly);
    check(collision.readAll() == "KEEP-EXISTING-COLLISION",
          "Atomic publication never overwrites a competing file");
    invalid("invalid directory",
            [&] { exportProtectedPdf(source, passwords(), output + "/missing/file.pdf"); });
    invalid("invalid extension",
            [&] { exportProtectedPdf(source, passwords(), output + "/cipher.txt"); });
    if (!qEnvironmentVariable("TATSU_DENIED_SAVE_DIR").isEmpty())
        invalid("NTFS denied write",
                [&]
                {
                    exportProtectedPdf(source, passwords(),
                                       qEnvironmentVariable("TATSU_DENIED_SAVE_DIR") +
                                           "/encrypted.pdf");
                });
    for (const auto& directory : QDir(output).entryList(
             {".pdf-tatsujin*", "pdf-tatsujin*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot))
        check(!directory.contains("save"), "No owned save candidate remains");
    return {{"rejected", rows}, {"atomic_collision_and_cancel", true}, {"volume_full", "未実行"}};
}
QJsonObject testEncryptionUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.openFile(fixtures + "/D01.pdf");
    window.doc.putSignature(0, "保護コピーのUI", {60, 80}, 12, Qt::black);
    window.refresh(true);
    window.show();
    const auto original = window.doc.pdf();
    const int cursor = window.doc.cursor;
    const auto revision = window.doc.revision;
    int ticks = 0, mode = 0;
    QString error;
    auto operate = [&]
    {
        int state = 0;
        QElapsedTimer deadline;
        deadline.start();
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(
            &timer, &QTimer::timeout,
            [&]
            {
                ++ticks;
                auto dialog = dynamic_cast<EncryptionDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                try
                {
                    check(deadline.elapsed() < 25000, "Encryption UI deadline");
                    auto field = [&](const char* name)
                    { return dialog->findChild<QLineEdit*>(name); };
                    auto save = dialog->findChild<QPushButton*>("encryptionSave");
                    if (state == 0)
                    {
                        dialog->resize(600, 480);
                        for (auto name : {"encryptionUser", "encryptionUserConfirm",
                                          "encryptionOwner", "encryptionOwnerConfirm"})
                            check(field(name)->echoMode() == QLineEdit::Password,
                                  "Passwords masked by default");
                        auto show = dialog->findChild<QCheckBox*>("encryptionShowPasswords");
                        show->setChecked(true);
                        check(field("encryptionUser")->echoMode() == QLineEdit::Normal,
                              "Explicit reveal");
                        show->setChecked(false);
                        field("encryptionUser")->setText(passwords().userPassword);
                        field("encryptionUserConfirm")->setText("mismatch");
                        field("encryptionOwner")->setText(passwords().ownerPassword);
                        field("encryptionOwnerConfirm")->setText(passwords().ownerPassword);
                        QTest::mouseClick(save, Qt::LeftButton);
                        check(save->isEnabled() && dialog->findChild<QLabel*>("encryptionMessage")
                                                       ->text()
                                                       .contains("一致"),
                              "Mismatch rejects without worker");
                        field("encryptionUserConfirm")->setText(passwords().userPassword);
                        field("encryptionPath")->setText(output + "/encryption-existing.pdf");
                        QTest::mouseClick(save, Qt::LeftButton);
                        state = 1;
                    }
                    else if (state == 1 && save->isEnabled())
                    {
                        check(dialog->findChild<QLabel*>("encryptionMessage")
                                  ->text()
                                  .contains("保存先"),
                              "Existing path fails and supports retry");
                        field("encryptionPath")
                            ->setText(output + QString("/encryption-ui-%1.pdf").arg(mode));
                        dialog->grab().save(output + QString("/encryption-ui-%1.png").arg(mode));
                        check(!save->visibleRegion().isEmpty() &&
                                  !dialog->findChild<QPushButton*>("encryptionCancel")
                                       ->visibleRegion()
                                       .isEmpty(),
                              "Actions visible at compact dimensions");
                        QTest::mouseClick(save, Qt::LeftButton);
                        state = 2;
                        if (mode == 1)
                        {
                            bool running = false;
                            for (auto thread : dialog->findChildren<QThread*>())
                                running |= thread->isRunning();
                            check(running,
                                  "Actual owned encryption worker started before cancellation");
                            dialog->reject();
                            state = 3;
                        }
                    }
                }
                catch (const std::exception& exception)
                {
                    error = QString::fromUtf8(exception.what());
                    dialog->reject();
                    state = 3;
                }
            });
        timer.start(3);
        window.exportEncryptedCopy();
        timer.stop();
        check(error.isEmpty(), error);
        check(window.doc.pdf() == original && window.doc.cursor == cursor &&
                  window.doc.revision == revision && window.doc.dirty() &&
                  window.doc.target.isEmpty(),
              "UI export never changes document state");
    };
    operate();
    check(QFileInfo::exists(output + "/encryption-ui-0.pdf"),
          "Actual dialog publishes encrypted copy");
    mode = 1;
    operate();
    check(!QFileInfo::exists(output + "/encryption-ui-1.pdf"), "Worker cancel publishes no file");
    window.undoAction->trigger();
    check(window.doc.cursor == cursor - 1, "Original Undo remains usable");
    window.redoAction->trigger();
    check(window.doc.pdf() == original, "Original Redo restores signature");
    window.doc.open(output + "/encryption-denied.pdf", passwords().userPassword);
    window.refresh(true);
    check(!window.doc.copyAllowed && !window.signatureAction->isEnabled() &&
              !window.ocrAction->isEnabled(),
          "Copy, editing and OCR disabled for denied encrypted PDF");
    QString printMessage;
    QTimer messages;
    QObject::connect(&messages, &QTimer::timeout,
                     [&]
                     {
                         if (auto box =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             printMessage = box->text();
                             box->accept();
                         }
                     });
    messages.start(5);
    QAction* printAction = nullptr;
    for (auto action : window.findChildren<QAction*>())
        if (action->shortcut() == QKeySequence(QKeySequence::Print))
            printAction = action;
    check(printAction != nullptr, "Product print action exists");
    printAction->trigger();
    check(printMessage.contains("印刷が許可"), "Actual print command enforces denied permission");
    QPrinter printer;
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(output + "/encryption-allowed-print.pdf");
    auto allowed = readPdf(output + "/encryption-rich-after.pdf", passwords().userPassword);
    printDocument(allowed, printer);
    check(QFileInfo(output + "/encryption-allowed-print.pdf").size() > 0,
          "Permitted decrypted pages print to PDF output");
    return {{"actual_dialog_mask_confirm_retry_save_cancel", true},
            {"source_Undo_Redo", true},
            {"print_permission_checked", true},
            {"GUI_event_ticks", ticks},
            {"native_UI", "未実行"}};
}
QJsonObject testEncryptionOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.history = {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6})};
    window.doc.saved = -1;
    window.doc.putSignature(0, "暗号化とOCRを保持", {30, 30}, 10, Qt::black);
    window.refresh(true);
    window.show();
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
    window.language->setCurrentIndex(0);
    window.scope->setCurrentIndex(0);
    window.startOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 90000),
          "Actual bilingual OCR before encryption");
    check(error.isEmpty(), error);
    writeCandidate(window.doc.pdf(), output + "/encryption-ocr-before.pdf");
    roundTrip(window.doc.pdf(), output + "/encryption-ocr-after.pdf", passwords());
    window.doc.open(output + "/encryption-ocr-after.pdf", passwords().userPassword);
    window.refresh(true);
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Fixed bilingual search terms retained");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    window.query->setText("市民公園");
    QTest::keyClick(window.query, Qt::Key_Return);
    auto search = window.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              15000),
          "Actual encrypted product search");
    window.canvas->setZoom(.5);
    window.canvas->goToPage(0);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "Encrypted OCR page ready");
    window.canvas->setFocus();
    const auto size = pageSize(window.doc.pdf().getCatalog()->getPage(0));
    const auto first = window.canvas->mapFromScene({1, 1}),
               last = window.canvas->mapFromScene({size.width() - 1, size.height() - 1});
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 60);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "Encrypted selection ready");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto copied = QApplication::clipboard()->text();
    check(copied.size() > 500 && copied.contains("市民公園"),
          "Actual Qt copy from authenticated encrypted OCR");
    return {{"actual_bilingual_OCR", true},
            {"encrypted_search_and_Qt_copy", true},
            {"copied_characters", copied.size()},
            {"pixel_difference", 0},
            {"OS_clipboard", "未実行"}};
}
} // namespace tatsu
