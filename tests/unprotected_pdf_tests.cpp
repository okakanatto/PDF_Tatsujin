#include "unprotected_pdf_tests.h"
#include "bookmark_edit.h"
#include "decryption_dialog.h"
#include "encrypted_pdf.h"
#include "form_fields.h"
#include "link_edit.h"
#include "page_decoration.h"
#include "page_operations.h"
#include "pdf_objects.h"
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
    fail("Invalid protection removal succeeded");
}
EncryptionOptions passwords()
{
    return {"閲覧-User-2026", "変更-Owner-2026", false, false, false};
}
struct ProtectedSnapshot
{
    PDFDocument document;
    PDFObject encryption;
    PDFSecurityHandler::AuthorizationResult role;
    EncryptionMode mode;
    uint32_t permissions = 0;
    explicit ProtectedSnapshot(const PDFDocument& input) : document(input)
    {
        const auto security = input.getStorage().getSecurityHandler();
        encryption = security->createEncryptionDictionaryObject();
        role = security->getAuthorizationResult();
        mode = security->getMode();
        for (uint32_t bit = 4; bit <= 2048; bit <<= 1)
            if (security->isAllowed(PDFSecurityHandler::Permission(bit)))
                permissions |= bit;
    }
    bool matches(const PDFDocument& input) const
    {
        const ProtectedSnapshot current(input);
        return document == input && document.getSourceDataHash() == input.getSourceDataHash() &&
               document.getInfo()->version.major == input.getInfo()->version.major &&
               document.getInfo()->version.minor == input.getInfo()->version.minor &&
               encryption == current.encryption && role == current.role && mode == current.mode &&
               permissions == current.permissions;
    }
};
PDFDocument encryptedInput(const PDFDocument& plain, const QString& path)
{
    const auto encrypted = protectPdf(plain, passwords());
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Create protected input");
    const auto data = encodePdf(encrypted);
    check(file.write(data) == data.size() && file.flush(), "Write protected input");
    file.close();
    return readPdf(path, passwords().userPassword);
}
void exactPages(PDFDocument before, PDFDocument after)
{
    check(before.getCatalog()->getPageCount() == after.getCatalog()->getPageCount(),
          "Page count retained");
    for (size_t index = 0; index < before.getCatalog()->getPageCount(); ++index)
    {
        check(pageSize(before.getCatalog()->getPage(index)) ==
                  pageSize(after.getCatalog()->getPage(index)),
              "Physical page geometry retained");
        check(renderPage(before, int(index), .6) == renderPage(after, int(index), .6),
              "Pixels retained exactly");
        check(pageText(before, int(index)) == pageText(after, int(index)), "Text retained exactly");
    }
}
} // namespace
QJsonObject testUnprotectedLifecycle(const QString& fixtures, const QString& output)
{
    Document rich;
    rich.history = {
        mergeDocuments({readPdf(fixtures + "/D02.pdf"), readPdf(fixtures + "/D07.pdf")})};
    rich.saved = -1;
    rich.putSignature(0, "保護解除後も再編集", {100, 250}, 12, Qt::black);
    QImage image(60, 40, QImage::Format_RGB32);
    image.fill(QColor("#629fe8"));
    rich.putImage(1, OverlayKind::Image, image, {70, 80}, 60);
    auto links = editableLinks(rich.pdf());
    links << LinkEntry{0, -1, {20, 20, 120, 35}, "フォームへ", LinkTarget::Page, 4, {}};
    rich.commit(replaceLinks(rich.pdf(), links));
    rich.commit(replaceBookmarks(rich.pdf(), {{{}, "日本語のしおり", -1, 4, true, true}}));
    DecorationOptions options;
    options.kind = DecorationKind::Watermark;
    options.watermark = "確認用";
    options.fontFamily = signatureFont();
    rich.commit(putDecoration(rich.pdf(), {0, 1}, options));
    for (const auto& field : formFields(rich.pdf()))
        if (field.qualifiedName == "name")
            putFormValue(rich, field.widget, {"髙橋 香織"});
    auto storage = rich.pdf().getStorage();
    auto trailer = *rich.pdf().getTrailerDictionary();
    auto catalog = *storage.getObject(trailer.get("Root")).getDictionary();
    PDFDictionary metadata;
    detail::set(metadata, "Type", PDFObject::createName("Metadata"));
    detail::set(metadata, "Subtype", PDFObject::createName("XML"));
    const auto metadataRef = storage.addObject(detail::streamObject(
        metadata,
        "<x:xmpmeta "
        "xmlns:x='adobe:ns:meta/'><secret>OWNER-COPY-METADATA-2026</secret></x:xmpmeta>"));
    detail::set(catalog, "Metadata", PDFObject::createReference(metadataRef));
    storage.setObject(trailer.get("Root").getReference(), detail::dictObject(catalog));
    auto info = *storage.getObject(trailer.get("Info")).getDictionary();
    detail::set(info, "TatsujinTest", PDFObject::createString("OWNER-COPY-INFO-2026"));
    storage.setObject(trailer.get("Info").getReference(), detail::dictObject(info));
    rich.commit(PDFDocument(std::move(storage), rich.pdf().getInfo()->version,
                            rich.pdf().getSourceDataHash()));
    writeCandidate(rich.pdf(), output + "/unprotected-rich-before.pdf");
    const auto input = output + "/unprotected-rich-input.pdf";
    auto encrypted = encryptedInput(rich.pdf(), input);
    const ProtectedSnapshot original(encrypted);
    const auto sourceHash = fileHash(input);
    const auto role = encrypted.getStorage().getSecurityHandler()->getAuthorizationResult();
    rejects([&] { unprotectPdf(encrypted, passwords().userPassword); });
    rejects([&] { unprotectPdf(encrypted, "wrong"); });
    check(encrypted.getStorage().getSecurityHandler()->getAuthorizationResult() == role &&
              !encrypted.getStorage().getSecurityHandler()->isAllowed(
                  PDFSecurityHandler::Permission::CopyContent),
          "Original user authentication and restrictions retained");
    const auto copy = output + "/unprotected-rich-copy.pdf";
    auto openedOwner = readPdf(input, passwords().ownerPassword);
    const ProtectedSnapshot ownerSnapshot(openedOwner);
    rejects([&] { unprotectPdf(openedOwner, "wrong"); });
    check(ownerSnapshot.matches(openedOwner),
          "Owner-opened document also requires correct password and remains unchanged");
    const auto hash = exportUnprotectedPdf(encrypted, passwords().ownerPassword, copy);
    Document editable;
    editable.open(copy);
    check(editable.readOnly.isEmpty() && editable.copyAllowed && !editable.dirty() &&
              hash == fileHash(copy),
          "Saved copy opens without password and is editable");
    exactPages(rich.pdf(), editable.pdf());
    const auto signature = signatures(editable.pdf(), 0).first();
    const int cursor = editable.cursor;
    editable.putSignature(0, "解除後の署名", signature.rect.topLeft(), signature.size,
                          signature.color, signature.ref, signature.fontFamily);
    editable.undo();
    check(signatures(editable.pdf(), 0).first().text == signature.text && editable.cursor == cursor,
          "Copy signature Undo");
    editable.redo();
    for (const auto& field : formFields(editable.pdf()))
        if (field.qualifiedName == "name")
            putFormValue(editable, field.widget, {"解除後の入力"});
    editable.save(output + "/unprotected-rich-edited.pdf");
    editable.open(editable.target);
    check(signatures(editable.pdf(), 0).first().text == "解除後の署名",
          "Saved copy signature reeditable");
    bool fieldEdited = false;
    for (const auto& field : formFields(editable.pdf()))
        if (field.qualifiedName == "name")
        {
            check(field.values == QStringList{"解除後の入力"}, "Saved form value");
            putFormValue(editable, field.widget, {"もう一度編集"});
            fieldEdited = true;
        }
    check(fieldEdited && original.matches(encrypted) && fileHash(input) == sourceHash &&
              !editingRestriction(encrypted).isEmpty(),
          "Original encrypted source unchanged, form reeditable");
    QFile manifest(fixtures + "/protection/manifest.json");
    check(manifest.open(QIODevice::ReadOnly), "Read independent protection fixtures");
    QJsonArray cases;
    for (const auto row : QJsonDocument::fromJson(manifest.readAll()).object()["cases"].toArray())
    {
        const auto fixture = row.toObject();
        const auto path = fixtures + "/protection/" + fixture["file"].toString();
        check(fileHash(path).toHex() == fixture["sha256"].toString().toLatin1(),
              "Frozen compatibility fixture hash");
        auto protectedPdf = readPdf(path, fixture["user_password"].toString());
        const ProtectedSnapshot snapshot(protectedPdf);
        rejects([&] { unprotectPdf(protectedPdf, "wrong"); });
        rejects([&] { unprotectPdf(protectedPdf, fixture["user_password"].toString()); });
        const auto target = output + "/unprotected-compat-" + fixture["file"].toString();
        exportUnprotectedPdf(protectedPdf, fixture["owner_password"].toString(), target);
        exactPages(protectedPdf, readPdf(target));
        check(snapshot.matches(protectedPdf),
              "Compatibility input objects, authentication and permissions retained");
        cases << QJsonObject{{"input", path},
                             {"copy", target},
                             {"revision", fixture["revision"]},
                             {"status", "PASS"}};
    }
    check(cases.size() == 6, "All independent compatibility cases executed");
    QFile record(output + "/unprotected-compatibility.json");
    check(record.open(QIODevice::WriteOnly), "Compatibility record");
    record.write(QJsonDocument(cases).toJson());
    return {{"owner_only", true},
            {"copy_signature_and_form_reediting", true},
            {"pixel_difference", 0},
            {"independent_input_cases", cases},
            {"original_unchanged", true}};
}
QJsonObject testUnprotectedFailures(const QString& fixtures, const QString& output)
{
    auto plain = readPdf(fixtures + "/D02.pdf");
    auto input = encryptedInput(plain, output + "/unprotected-failure-input.pdf");
    const ProtectedSnapshot snapshot(input);
    const auto sourceHash = fileHash(output + "/unprotected-failure-input.pdf");
    QJsonArray rows;
    auto invalid = [&](QString name, const std::function<void()>& operation)
    {
        rows << QJsonObject{{"case", name}, {"error", rejects(operation)}};
        check(snapshot.matches(input) &&
                  fileHash(output + "/unprotected-failure-input.pdf") == sourceHash,
              "Failed export preserves source objects, encryption, authentication, permissions and "
              "file hash");
    };
    invalid("empty", [&] { unprotectPdf(PDFDocument{}, "owner"); });
    invalid("unencrypted", [&] { unprotectPdf(plain, "owner"); });
    invalid("empty wrong owner", [&] { unprotectPdf(input, {}); });
    invalid("oversized password", [&] { unprotectPdf(input, QString(1025, 'x')); });
    invalid("invalid Unicode", [&] { unprotectPdf(input, QString(QChar(0xd800))); });
    for (const auto& name : {"D08-signed.pdf", "D08-xfa.pdf"})
    {
        auto restricted = readPdf(fixtures + "/" + name);
        auto storage = restricted.getStorage();
        storage.setSecurityHandler(
            PDFSecurityHandlerPointer(input.getStorage().getSecurityHandler()->clone()));
        auto trailer = *restricted.getTrailerDictionary();
        detail::set(trailer, "Encrypt",
                    input.getObject(input.getTrailerDictionary()->get("Encrypt")));
        storage.setTrailerDictionary(detail::dictObject(trailer));
        PDFDocument encryptedRestricted(std::move(storage), restricted.getInfo()->version,
                                        restricted.getSourceDataHash());
        invalid(name, [&] { unprotectPdf(encryptedRestricted, passwords().ownerPassword); });
    }
    auto cryptStorage = input.getStorage();
    bool cryptFound = false;
    for (auto& entry : cryptStorage.getObjects())
        if (entry.object.isStream())
        {
            auto dictionary = *entry.object.getStream()->getDictionary();
            detail::set(dictionary, "Filter", detail::arrObject({PDFObject::createName("Crypt")}));
            entry.object =
                detail::streamObject(dictionary, *entry.object.getStream()->getContent());
            cryptFound = true;
            break;
        }
    check(cryptFound, "Actual crypt stream synthesized");
    PDFDocument crypt(std::move(cryptStorage), input.getInfo()->version, input.getSourceDataHash());
    invalid("explicit Crypt filter", [&] { unprotectPdf(crypt, passwords().ownerPassword); });
    const auto existing = output + "/unprotected-existing.pdf";
    writeCandidate(plain, existing);
    const auto hash = fileHash(existing);
    invalid("existing destination",
            [&] { exportUnprotectedPdf(input, passwords().ownerPassword, existing); });
    check(fileHash(existing) == hash, "Existing file unchanged");
    invalid("source destination",
            [&]
            {
                exportUnprotectedPdf(input, passwords().ownerPassword,
                                     output + "/unprotected-failure-input.pdf");
            });
    invalid("invalid directory",
            [&] {
                exportUnprotectedPdf(input, passwords().ownerPassword, output + "/missing/new.pdf");
            });
    invalid("invalid extension",
            [&] { exportUnprotectedPdf(input, passwords().ownerPassword, output + "/new.txt"); });
    for (int mode = 0; mode < 2; ++mode)
    {
        bool cancelled = false;
        const auto destination = output + QString("/unprotected-cancel-%1.pdf").arg(mode);
        invalid(mode == 0 ? "initial cancellation" : "cancel after candidate written",
                [&]
                {
                    exportUnprotectedPdf(
                        input, passwords().ownerPassword, destination,
                        [&] { return mode == 0 || cancelled; },
                        [&](QString text)
                        {
                            if (text.startsWith("編集用コピーを保存"))
                                cancelled = true;
                        });
                });
        check(!QFileInfo::exists(destination), "Cancelled publication does not exist");
        if (mode == 1)
            check(cancelled, "Candidate was written and verified before cancel");
    }
    const auto race = output + "/unprotected-race.pdf";
    invalid("publication collision",
            [&]
            {
                exportUnprotectedPdf(
                    input, passwords().ownerPassword, race, {},
                    [&](QString text)
                    {
                        if (text.startsWith("編集用コピーを保存"))
                        {
                            QFile file(race);
                            check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                                  "Actual target collision");
                            file.write("KEEP-UNPROTECTED-COLLISION");
                        }
                    });
            });
    QFile collision(race);
    check(collision.open(QIODevice::ReadOnly) &&
              collision.readAll() == "KEEP-UNPROTECTED-COLLISION",
          "Competing target retained");
    const auto denied = qEnvironmentVariable("TATSU_DENIED_SAVE_DIR");
    if (!denied.isEmpty())
        invalid("NTFS write denial",
                [&] {
                    exportUnprotectedPdf(input, passwords().ownerPassword,
                                         denied + "/unprotected.pdf");
                });
    return {{"rejected_cases", rows},
            {"NTFS_denial", denied.isEmpty() ? "未実行" : "PASS"},
            {"volume_full", "未実行"},
            {"original_unchanged", true}};
}
QJsonObject testUnprotectedUi(const QString& fixtures, const QString& output)
{
    Window original;
    auto plain = readPdf(fixtures + "/D02.pdf");
    encryptedInput(plain, output + "/unprotected-ui-input.pdf");
    original.doc.open(output + "/unprotected-ui-input.pdf", passwords().userPassword);
    original.refresh(true);
    original.show();
    auto action = original.findChild<QAction*>("createEditableCopy");
    check(action && action->isEnabled() && !original.signatureAction->isEnabled(),
          "Only explicit copy action enabled on encrypted document");
    const auto hash = fileHash(original.doc.source);
    const auto revision = original.doc.revision;
    int mode = 0, ticks = 0;
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
                auto dialog = dynamic_cast<DecryptionDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                try
                {
                    check(deadline.elapsed() < 25000, "Owner copy UI deadline");
                    auto save = dialog->findChild<QPushButton*>("decryptionSave");
                    auto consent = dialog->findChild<QCheckBox*>("decryptionConsent");
                    auto password = dialog->findChild<QLineEdit*>("decryptionPassword");
                    if (state == 0)
                    {
                        dialog->resize(600, 400);
                        check(!consent->isChecked() && !save->isEnabled() &&
                                  password->echoMode() == QLineEdit::Password,
                              "Explicit consent and masked defaults");
                        dialog->accept();
                        check(dialog->isVisible(),
                              "Direct accept does not bypass unchecked choice");
                        auto reveal = dialog->findChild<QCheckBox*>("decryptionShowPassword");
                        reveal->setChecked(true);
                        check(password->echoMode() == QLineEdit::Normal,
                              "Explicit password reveal");
                        reveal->setChecked(false);
                        consent->setChecked(true);
                        password->setText(passwords().userPassword);
                        dialog->findChild<QLineEdit*>("decryptionPath")
                            ->setText(output + QString("/unprotected-ui-%1.pdf").arg(mode));
                        QTest::mouseClick(save, Qt::LeftButton);
                        state = 1;
                    }
                    else if (state == 1 && save->isEnabled())
                    {
                        check(dialog->findChild<QLabel*>("decryptionMessage")
                                  ->text()
                                  .contains("変更権限"),
                              "User password rejected with retry");
                        password->setText(passwords().ownerPassword);
                        dialog->grab().save(output + QString("/unprotected-ui-%1.png").arg(mode));
                        check(!save->visibleRegion().isEmpty() &&
                                  !dialog->findChild<QPushButton*>("decryptionCancel")
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
                            check(running, "Owned worker started before cancellation");
                            dialog->reject();
                        }
                    }
                }
                catch (const std::exception& exception)
                {
                    error = QString::fromUtf8(exception.what());
                    dialog->reject();
                }
            });
        timer.start(3);
        action->trigger();
        timer.stop();
        check(error.isEmpty(), error);
        check(original.doc.revision == revision && fileHash(original.doc.source) == hash &&
                  !original.doc.readOnly.isEmpty() && !original.doc.copyAllowed &&
                  !original.doc.dirty(),
              "Original window unchanged and restricted");
    };
    operate();
    Window* created = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "unprotectedCreatedDocument")
            created = dynamic_cast<Window*>(widget);
    check(created && created->doc.readOnly.isEmpty() && created->signatureAction->isEnabled() &&
              created->ocrAction->isEnabled(),
          "Saved editable copy opened in separate document window");
    created->doc.putSignature(0, "コピーで編集", {70, 200}, 12, Qt::black);
    created->refresh();
    created->undoAction->trigger();
    check(created->doc.cursor == 0 && !created->doc.dirty(), "Created window Undo usable");
    created->redoAction->trigger();
    check(signatures(created->doc.pdf(), 0).last().text == "コピーで編集",
          "Created window Redo usable");
    created->doc.save(output + "/unprotected-ui-edited.pdf");
    delete created;
    mode = 1;
    operate();
    check(!QFileInfo::exists(output + "/unprotected-ui-1.pdf"),
          "UI cancel creates no copy or window");
    original.doc.open(fixtures + "/D01.pdf");
    original.refresh(true);
    check(!action->isEnabled(), "Unencrypted document has no active removal command");
    return {{"explicit_consent_mask_reveal_reject_retry", true},
            {"separate_editable_window_Undo_Redo_save", true},
            {"owned_worker_cancel", true},
            {"GUI_event_ticks", ticks},
            {"native_UI", "未実行"}};
}
QJsonObject testUnprotectedOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.history = {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6})};
    window.doc.saved = -1;
    window.doc.putSignature(0, "解除してもOCR保持", {30, 30}, 10, Qt::black);
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
          "Actual OCR before explicit copy");
    check(error.isEmpty(), error);
    writeCandidate(window.doc.pdf(), output + "/unprotected-ocr-before.pdf");
    auto encrypted = encryptedInput(window.doc.pdf(), output + "/unprotected-ocr-input.pdf");
    exportUnprotectedPdf(encrypted, passwords().ownerPassword,
                         output + "/unprotected-ocr-copy.pdf");
    window.doc.open(output + "/unprotected-ocr-copy.pdf");
    window.refresh(true);
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Fixed bilingual OCR terms");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    window.query->setText("市民公園");
    QTest::keyClick(window.query, Qt::Key_Return);
    auto search = window.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              15000),
          "Actual search in owner copy");
    window.canvas->setZoom(.5);
    window.canvas->goToPage(0);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "Copy OCR page ready");
    const auto size = pageSize(window.doc.pdf().getCatalog()->getPage(0));
    const auto first = window.canvas->mapFromScene({1, 1}),
               last = window.canvas->mapFromScene({size.width() - 1, size.height() - 1});
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 60);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "Actual copy selection ready");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto copied = QApplication::clipboard()->text();
    check(copied.size() > 500 && copied.contains("市民公園"),
          "Actual Qt copy of OCR text after removal");
    const auto signature = signatures(window.doc.pdf(), 0).first();
    window.doc.moveSignature(0, signature, {10, 5});
    window.doc.undo();
    window.doc.redo();
    window.doc.save(output + "/unprotected-ocr-edited.pdf");
    return {{"actual_bilingual_OCR_search_Qt_copy", true},
            {"copied_characters", copied.size()},
            {"signature_move_Undo_Redo_save", true},
            {"OS_clipboard", "未実行"}};
}
} // namespace tatsu
