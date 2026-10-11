#include "certificate_signing_tests.h"
#include "certificate_signing.h"
#include "certificate_signing_dialog.h"
#include "form_design.h"
#include "form_fields.h"
#include "window.h"
#include <QtTest/QTest>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

namespace tatsu
{
namespace
{
void check(bool ok, const QString& text)
{
    if (!ok)
        fail(text);
}
template <typename T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
QByteArray encodeP12(PKCS12* p12)
{
    const auto length = i2d_PKCS12(p12, nullptr);
    check(length > 0, "Encode synthetic P12 length");
    QByteArray bytes(length, Qt::Uninitialized);
    auto cursor = reinterpret_cast<unsigned char*>(bytes.data());
    check(i2d_PKCS12(p12, &cursor) == length, "Encode synthetic P12");
    return bytes;
}
QByteArray makeKey(bool ec, const QString& password, const char* first = "20200101000000Z",
                   const char* last = "20350101000000Z", bool signingUsage = true,
                   bool weak = false, bool multiple = false, bool mismatched = false,
                   bool legacy = false)
{
    Owned<EVP_PKEY, EVP_PKEY_free> key(
        ec ? EVP_EC_gen("prime256v1") : EVP_RSA_gen(weak ? 1024 : 2048), EVP_PKEY_free);
    Owned<X509, X509_free> certificate(X509_new(), X509_free);
    check(key && certificate && X509_set_version(certificate.get(), 2) == 1 &&
              ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 42) == 1 &&
              X509_set_pubkey(certificate.get(), key.get()) == 1,
          "Create synthetic certificate");
    check(ASN1_TIME_set_string_X509(X509_getm_notBefore(certificate.get()), first) == 1 &&
              ASN1_TIME_set_string_X509(X509_getm_notAfter(certificate.get()), last) == 1,
          "Set fixed certificate periods");
    const auto common = QString("日本語署名の合成試験").toUtf8();
    auto name = X509_get_subject_name(certificate.get());
    check(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                     reinterpret_cast<const unsigned char*>(common.constData()),
                                     common.size(), -1, 0) == 1 &&
              X509_set_issuer_name(certificate.get(), name) == 1,
          "Set Japanese certificate name");
    Owned<X509_EXTENSION, X509_EXTENSION_free> usage(
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_key_usage,
                            signingUsage ? "critical,digitalSignature"
                                         : "critical,keyEncipherment"),
        X509_EXTENSION_free);
    check(usage && X509_add_ext(certificate.get(), usage.get(), -1) == 1 &&
              X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0,
          "Sign synthetic certificate");
    const auto pass = password.toUtf8();
    Owned<PKCS12, PKCS12_free> p12(
        PKCS12_create(pass.constData(), "Synthetic signing test only", key.get(), certificate.get(),
                      nullptr, legacy ? NID_pbe_WithSHA1And3_Key_TripleDES_CBC : 0, 0, 2048, 2048,
                      0),
        PKCS12_free);
    check(bool(p12), "Create modern encrypted synthetic P12");
    if (mismatched)
    {
        Owned<EVP_PKEY, EVP_PKEY_free> wrong(EVP_EC_gen("prime256v1"), EVP_PKEY_free);
        STACK_OF(PKCS12_SAFEBAG)* bags = nullptr;
        STACK_OF(PKCS7)* safes = nullptr;
        check(wrong &&
                  PKCS12_add_key(&bags, wrong.get(), 0, 2048, NID_aes_256_cbc, pass.constData()) &&
                  PKCS12_add_cert(&bags, certificate.get()) &&
                  PKCS12_add_safe(&safes, bags, -1, 0, nullptr) == 1,
              "Build valid container with a deliberately mismatched private key");
        p12.reset(PKCS12_add_safes(safes, NID_pkcs7_data));
        check(p12 && PKCS12_set_mac(p12.get(), pass.constData(), pass.size(), nullptr, 0, 2048,
                                    EVP_sha256()) == 1,
              "Authenticate mismatched synthetic P12");
        sk_PKCS12_SAFEBAG_pop_free(bags, PKCS12_SAFEBAG_free);
        sk_PKCS7_pop_free(safes, PKCS7_free);
    }
    if (multiple)
    {
        auto safes = PKCS12_unpack_authsafes(p12.get());
        auto bags = sk_PKCS12_SAFEBAG_new_null();
        auto bag = PKCS12_SAFEBAG_create0_p8inf(EVP_PKEY2PKCS8(key.get()));
        check(safes && bags && bag && sk_PKCS12_SAFEBAG_push(bags, bag) == 1,
              "Create second synthetic key bag");
        auto safe = PKCS12_pack_p7data(bags);
        check(safe && sk_PKCS7_push(safes, safe) > 0 &&
                  PKCS12_pack_authsafes(p12.get(), safes) == 1 &&
                  PKCS12_set_mac(p12.get(), pass.constData(), pass.size(), nullptr, 0, 2048,
                                 EVP_sha256()) == 1,
              "Package multiple key P12 with valid MAC");
        sk_PKCS12_SAFEBAG_pop_free(bags, PKCS12_SAFEBAG_free);
        sk_PKCS7_pop_free(safes, PKCS7_free);
    }
    return encodeP12(p12.get());
}
template <typename F> QString rejects(F function)
{
    try
    {
        function();
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
    fail("Unsafe certificate signing unexpectedly succeeded");
}
} // namespace
QJsonObject testCertificateSigningFoundation(const QString& fixtures, const QString& output)
{
    const QString password = "署名の試験パスワード";
    const auto rsa = makeKey(false, password), ec = makeKey(true, password);
    Document document;
    document.open(fixtures + "/D07.pdf");
    for (const auto& field : formFields(document.pdf()))
        if (field.qualifiedName == "name")
            putFormValue(document, field.widget, {"髙橋 香織"});
    document.putSignature(0, "保持する未保存の日本語署名", {90, 230}, 12, Qt::blue);
    QVector<FormDesignEntry> design;
    const FormKind kinds[] = {FormKind::Text,  FormKind::Multiline, FormKind::Checkbox,
                              FormKind::Radio, FormKind::Combo,     FormKind::List};
    for (int i = 0; i < 6; ++i)
    {
        FormDesignEntry field;
        field.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        field.name = QString("signed-design-%1").arg(i + 1);
        field.caption = "署名前の設計項目";
        field.kind = kinds[i];
        field.widgets << FormDesignWidget{
            QUuid::createUuid().toString(QUuid::WithoutBraces), 0, {310, 90.0 + i * 75.0, 220, 45}};
        if (i < 2)
            field.values = {i == 0 ? "髙橋 𠮷野" : "複数行の署名試験\n山田 太郎"};
        else
        {
            field.exports = {"第一", "第二"};
            field.labels = {"第一の選択肢", "第二の選択肢"};
            field.values = {"第二"};
            if (i == 2)
                field.exports = field.labels = field.values = {"同意"};
            if (i == 3)
                field.widgets << FormDesignWidget{
                    QUuid::createUuid().toString(QUuid::WithoutBraces), 0, {310, 340, 220, 25}};
            if (i == 5)
                field.multiple = true;
        }
        design << field;
    }
    document.commit(replaceFormDesign(document.pdf(), design));
    const auto original = encodePdf(document.pdf());
    writeCandidate(document.pdf(), output + "/certificate-signing-before.pdf");
    const auto cursor = document.cursor, saved = document.saved;
    const auto fields = formFields(document.pdf());
    QJsonArray successes;
    for (const auto& item : {std::pair{QString("RSA"), rsa}, std::pair{QString("EC"), ec}})
    {
        const auto identity = inspectSigningCertificate(item.second, password);
        check(identity.subject.contains("日本語署名の合成試験"),
              "Japanese certificate identity is readable");
        const auto path = output + "/certificate-signed-" + item.first + ".pdf";
        const auto hash =
            exportSignedPdf(document.pdf(), item.second, password, "日英文書の承認署名 🖋", path);
        const auto result = verifyCertificateSignatures(path, hash, {false, {}});
        check(result.signatures.size() == 1 &&
                  result.signatures.first().integrity == SignatureIntegrity::Unchanged &&
                  result.signatures.first().entireFile &&
                  result.signatures.first().certificate.fingerprint == identity.fingerprint,
              "Created PDF is mathematically signed by selected certificate");
        auto reopened = readPdf(path);
        check(reopened.getCatalog()->getPageCount() == document.pages() &&
                  !editingRestriction(reopened).isEmpty() && signatures(reopened, 0).size() == 1 &&
                  signatures(reopened, 0).first().text == "保持する未保存の日本語署名",
              "Signed copy retains pages and unsaved visible signature, becomes read-only");
        const auto after = formFields(reopened);
        for (const auto& field : fields)
        {
            const auto match = std::find_if(
                after.begin(), after.end(), [&](const FormField& row)
                { return row.qualifiedName == field.qualifiedName && row.widget == field.widget; });
            check(match != after.end() && match->values == field.values &&
                      match->kind == field.kind && match->rectangle == field.rectangle,
                  "Existing form remains intact: " + field.qualifiedName);
        }
        check(
            renderPage(reopened, 0, 1).save(output + "/certificate-signed-" + item.first + ".png"),
            "Render actual signed PDF");
        check(encodePdf(document.pdf()) == original && document.cursor == cursor &&
                  document.saved == saved && document.dirty(),
              "Signing a copy preserves original and unsaved history");
        successes << QJsonObject{
            {"algorithm", item.first},
            {"file", QFileInfo(path).fileName()},
            {"certificate_sha256", identity.fingerprint},
            {"source_sha256",
             QString::fromLatin1(
                 QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex())}};
    }
    QJsonArray failures;
    auto rejected = [&](const QString& condition, auto function)
    { failures << QJsonObject{{"condition", condition}, {"error", rejects(function)}}; };
    rejected("wrong password", [&] { inspectSigningCertificate(rsa, "wrong"); });
    // Fixture generation must succeed outside the rejection wrapper: a broken
    // generator is a failing test, never evidence that the product rejected it.
    auto invalidKey = [&](const QString& condition, const QByteArray& bytes)
    { rejected(condition, [&] { inspectSigningCertificate(bytes, password); }); };
    invalidKey(
        "private key differs from certificate",
        makeKey(false, password, "20200101000000Z", "20350101000000Z", true, false, false, true));
    invalidKey("legacy P12 cipher unsupported",
               makeKey(false, password, "20200101000000Z", "20350101000000Z", true, false, false,
                       false, true));
    invalidKey("broken P12", "not a P12");
    invalidKey("oversized P12", QByteArray(4 * 1024 * 1024 + 1, 'x'));
    invalidKey("multiple private keys",
               makeKey(false, password, "20200101000000Z", "20350101000000Z", true, false, true));
    invalidKey("weak key",
               makeKey(false, password, "20200101000000Z", "20350101000000Z", true, true));
    invalidKey("expired", makeKey(false, password, "20200101000000Z", "20210101000000Z"));
    invalidKey("future", makeKey(false, password, "20350101000000Z", "20360101000000Z"));
    invalidKey("signing KeyUsage forbidden",
               makeKey(false, password, "20200101000000Z", "20350101000000Z", false));
    rejected("existing signed PDF",
             [&] { prepareSignedPdf(readPdf(fixtures + "/D08-signed.pdf"), rsa, password, {}); });
    rejected("encrypted PDF",
             [&]
             {
                 prepareSignedPdf(readPdf(fixtures + "/D08-encrypted.pdf", "correct-password"), rsa,
                                  password, {});
             });
    rejected("existing destination",
             [&] {
                 exportSignedPdf(document.pdf(), rsa, password, {},
                                 output + "/certificate-signed-RSA.pdf");
             });
    rejected("cancel before signing",
             [&] { prepareSignedPdf(document.pdf(), rsa, password, {}, [] { return true; }); });
    int checkpoints = 0;
    prepareSignedPdf(document.pdf(), rsa, password, {},
                     [&]
                     {
                         ++checkpoints;
                         return false;
                     });
    int observed = 0;
    rejected("cancel after signing before reflection",
             [&]
             {
                 prepareSignedPdf(document.pdf(), rsa, password, {},
                                  [&] { return ++observed >= checkpoints; });
             });
    auto mixed = readPdf(fixtures + "/D02.pdf");
    writeCandidate(mixed, output + "/certificate-geometry-before.pdf");
    exportSignedPdf(mixed, ec, password, "回転・混在ページを保持",
                    output + "/certificate-geometry-signed.pdf");
    auto geometry = readPdf(output + "/certificate-geometry-signed.pdf");
    for (int page = 0; page < mixed.getCatalog()->getPageCount(); ++page)
        check(pageSize(mixed.getCatalog()->getPage(page)) ==
                      pageSize(geometry.getCatalog()->getPage(page)) &&
                  renderPage(mixed, page, .5) == renderPage(geometry, page, .5),
              "All rotated/CropBox/UserUnit page renders preserved");
    check(encodePdf(document.pdf()) == original && document.cursor == cursor &&
              document.saved == saved && document.sourceHash == fileHash(fixtures + "/D07.pdf"),
          "All failures preserve original and history");
    QFile record(output + "/certificate-signing-foundation.json");
    check(record.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Fresh signing result record");
    record.write(
        QJsonDocument(
            QJsonObject{
                {"successes", successes},
                {"rejections", failures},
                {"private_key_files_written", false},
                {"scope", "Synthetic foundation; user-facing signing workflow not tested here"}})
            .toJson());
    return {{"RSA_EC_signed_copies", true},
            {"form_values_geometry_and_unsaved_signature_preserved", true},
            {"rejections", failures.size()},
            {"private_key_files_written", false},
            {"original_history_unchanged", true}};
}
QJsonObject testCertificateSigningUi(const QString& fixtures, const QString& output)
{
    const QString password = "合成試験専用のパスワード";
    auto keys = privateTemporaryDirectory(output + "/synthetic-signing-key-XXXXXX");
    check(keys->isValid(), "Private synthetic key directory");
    const auto keyPath = keys->filePath("synthetic.p12");
    {
        QFile file(keyPath);
        const auto bytes = makeKey(false, password);
        check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                  file.write(bytes) == bytes.size(),
              "Write encrypted synthetic key for actual file-picker workflow");
    }
    Window original;
    auto action = original.findChild<QAction*>("exportSignedCertificateCopy");
    check(action && !action->isEnabled(), "Signing requires a document");
    original.openFile(fixtures + "/D07.pdf");
    original.doc.putSignature(0, "元の未保存署名を保持", {90, 230}, 12, Qt::black);
    original.refresh();
    original.show();
    const auto body = encodePdf(original.doc.pdf());
    const auto cursor = original.doc.cursor, saved = original.doc.saved;
    const auto hash = original.doc.sourceHash;
    check(action->isEnabled(), "Unsaved editable document supports signed copy");

    CertificateSigningDialog preview(original.doc.pdf(), {}, output + "/unused-preview.pdf");
    preview.resize(620, 440);
    preview.show();
    auto path = preview.findChild<QLineEdit*>("signingKeyPath");
    auto pass = preview.findChild<QLineEdit*>("signingPassword");
    auto inspect = preview.findChild<QPushButton*>("inspectSigningCertificate");
    auto consent = preview.findChild<QCheckBox*>("signingConsent");
    auto save = preview.findChild<QPushButton*>("saveSignedCertificateCopy");
    auto progress = preview.findChild<QProgressBar*>("signingProgress");
    auto identity = preview.findChild<QPlainTextEdit*>("signingCertificateIdentity");
    check(!save->isEnabled() && !consent->isEnabled() && pass->echoMode() == QLineEdit::Password,
          "No implicit consent; password initially masked");
    path->setText(keyPath);
    pass->setText("wrong");
    inspect->click();
    check(QTest::qWaitFor([&] { return !progress->isVisible(); }, 30000) && !consent->isEnabled() &&
              identity->toPlainText().isEmpty() && !save->isEnabled(),
          "Wrong password supports retry");
    pass->setText(password);
    inspect->click();
    check(QTest::qWaitFor([&] { return consent->isEnabled(); }, 30000),
          "Actual P12 inspection worker");
    check(identity->toPlainText().contains("日本語署名の合成試験"),
          "Readable Japanese certificate");
    consent->setChecked(true);
    check(save->isEnabled(), "Explicit consent enables save");
    preview.findChild<QPlainTextEdit*>("signingReason")->setPlainText("理由を変更");
    check(!consent->isChecked() && !save->isEnabled(), "Changed reason requires renewed consent");
    consent->setChecked(true);
    preview.findChild<QLineEdit*>("signingDestination")->setText(output + "/unused-preview-2.pdf");
    check(!consent->isChecked(), "Changed destination requires renewed consent");
    pass->setText(password + "変更");
    check(!consent->isEnabled() && identity->toPlainText().isEmpty(),
          "Changed password invalidates identity");
    pass->setText(password);
    inspect->click();
    check(QTest::qWaitFor([&] { return consent->isEnabled(); }, 30000), "Retry inspection");
    auto scroll = preview.findChild<QScrollArea*>();
    check(scroll && scroll->verticalScrollBar()->maximum() > 0, "Compact dialog settings scroll");
    scroll->ensureWidgetVisible(consent);
    check(preview.grab().save(output + "/certificate-signing-compact.png"),
          "Actual compact screenshot");
    QTest::keyClick(&preview, Qt::Key_Escape);
    check(!preview.isVisible() && !QFileInfo::exists(output + "/unused-preview-2.pdf"),
          "Escape preview makes no copy");

    QString error;
    int state = 0, ticks = 0;
    QTimer timer;
    timer.setInterval(5);
    QObject::connect(
        &timer, &QTimer::timeout, &original,
        [&]
        {
            ++ticks;
            auto dialog = dynamic_cast<CertificateSigningDialog*>(
                original.findChild<QDialog*>("certificateSigningDialog"));
            if (!dialog)
                return;
            try
            {
                if (state == 0)
                {
                    dialog->findChild<QLineEdit*>("signingKeyPath")->setText(keyPath);
                    dialog->findChild<QLineEdit*>("signingPassword")->setText(password);
                    dialog->findChild<QPushButton*>("inspectSigningCertificate")->click();
                    state = 1;
                }
                else if (state == 1 && dialog->findChild<QCheckBox*>("signingConsent")->isEnabled())
                {
                    dialog->findChild<QLineEdit*>("signingDestination")
                        ->setText(output + "/certificate-ui-signed.pdf");
                    dialog->findChild<QPlainTextEdit*>("signingReason")
                        ->setPlainText("画面で明示確認する署名 🖋");
                    auto agree = dialog->findChild<QCheckBox*>("signingConsent");
                    agree->setFocus();
                    QTest::keyClick(agree, Qt::Key_Space);
                    check(agree->isChecked(), "Keyboard confirms signing consent");
                    check(dialog->grab().save(output + "/certificate-signing-confirmed.png"),
                          "Actual signing screen");
                    dialog->findChild<QPushButton*>("saveSignedCertificateCopy")->click();
                    state = 2;
                }
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
                         error = "Actual signing UI timed out";
                         if (auto dialog = original.findChild<QDialog*>("certificateSigningDialog"))
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
        if (widget->objectName() == "signedCertificateCreatedDocument")
            created = dynamic_cast<Window*>(widget);
    std::unique_ptr<Window> owned(created);
    check(state == 2 && created && !created->doc.readOnly.isEmpty() &&
              !created->signatureAction->isEnabled() && !created->ocrAction->isEnabled(),
          "Created signed copy opens in a read-only window");
    check(verifyCertificateSignatures(created->doc.source, created->doc.sourceHash, {false, {}})
                  .signatures.first()
                  .integrity == SignatureIntegrity::Unchanged,
          "Actual UI output has a correct signature");
    check(encodePdf(original.doc.pdf()) == body && original.doc.cursor == cursor &&
              original.doc.saved == saved && original.doc.sourceHash == hash &&
              original.doc.dirty(),
          "Original document and history unchanged");
    original.undoAction->trigger();
    check(signatures(original.doc.pdf(), 0).isEmpty(),
          "Original Undo remains usable after signing");
    original.redoAction->trigger();
    check(encodePdf(original.doc.pdf()) == body, "Original Redo restores unchanged snapshot");
    CertificateSigningDialog cancelled(original.doc.pdf(), {},
                                       output + "/certificate-ui-cancelled.pdf");
    cancelled.show();
    cancelled.findChild<QLineEdit*>("signingKeyPath")->setText(keyPath);
    cancelled.findChild<QLineEdit*>("signingPassword")->setText(password);
    cancelled.findChild<QPushButton*>("inspectSigningCertificate")->click();
    check(
        QTest::qWaitFor(
            [&] { return cancelled.findChild<QCheckBox*>("signingConsent")->isEnabled(); }, 30000),
        "Prepare cancellation");
    cancelled.findChild<QCheckBox*>("signingConsent")->setChecked(true);
    cancelled.findChild<QPushButton*>("saveSignedCertificateCopy")->click();
    QTest::keyClick(&cancelled, Qt::Key_Escape);
    check(QTest::qWaitFor([&] { return !cancelled.isVisible(); }, 30000) &&
              cancelled.savedPath().isEmpty() &&
              !QFileInfo::exists(output + "/certificate-ui-cancelled.pdf"),
          "Escape cancels running signing worker without publication");
    const auto temporary = keys->path();
    keys.reset();
    check(!QFileInfo::exists(temporary), "Synthetic encrypted key cleaned after UI test");
    original.doc.saved = original.doc.cursor;
    return {{"actual_menu_inspect_consent_save_and_separate_window", true},
            {"original_Undo_Redo", true},
            {"mask_retry_invalidation_compact_scroll_and_Escape", true},
            {"event_ticks", ticks},
            {"synthetic_key_removed", true},
            {"native_IME_clipboard_OS_DPI", "未実行"}};
}

QJsonObject testCertificateSigningAtomic(const QString& fixtures, const QString& output)
{
    const QString password = "atomic synthetic password";
    const auto key = makeKey(true, password);
    const auto document = readPdf(fixtures + "/D07.pdf");
    const auto before = encodePdf(document);
    QJsonArray errors;
    auto rejected = [&](const QString& name, auto operation)
    { errors << QJsonObject{{"condition", name}, {"error", rejects(operation)}}; };
    const auto destination = output + "/certificate-atomic.pdf";
    rejected("input changed before signing",
             [&]
             {
                 exportSignedPdf(document, key, password, {}, destination, {}, {},
                                 [] { fail("Synthetic source update"); });
             });
    int validations = 0;
    rejected("input changed before publication",
             [&]
             {
                 exportSignedPdf(document, key, password, {}, destination, {}, {},
                                 [&]
                                 {
                                     if (++validations == 2)
                                         fail("Synthetic source update before publication");
                                 });
             });
    check(validations == 2 && !QFileInfo::exists(destination),
          "Inputs checked at both transaction boundaries");
    const auto rival = output + "/certificate-rival.pdf";
    rejected("destination created during signing",
             [&]
             {
                 exportSignedPdf(document, key, password, {}, rival, {},
                                 [&](QString message)
                                 {
                                     if (message.contains("新しいPDF"))
                                     {
                                         QFile file(rival);
                                         check(
                                             file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                                                 file.write("rival original") == 14,
                                             "Create owned racing destination");
                                     }
                                 });
             });
    QFile kept(rival);
    check(kept.open(QIODevice::ReadOnly) && kept.readAll() == "rival original",
          "Racing destination preserved byte for byte");
    auto decoded = reinterpret_cast<const unsigned char*>(key.constData());
    Owned<PKCS12, PKCS12_free> parsed(d2i_PKCS12(nullptr, &decoded, key.size()), PKCS12_free);
    const ASN1_INTEGER* iterations = nullptr;
    PKCS12_get0_mac(nullptr, nullptr, nullptr, &iterations, parsed.get());
    check(iterations && ASN1_INTEGER_set(const_cast<ASN1_INTEGER*>(iterations), 1000001) == 1,
          "Synthetic excessive MAC iteration header");
    rejected("MAC iterations exceed resource cap",
             [&] { inspectSigningCertificate(encodeP12(parsed.get()), password); });
    auto safes = PKCS12_unpack_authsafes(parsed.get());
    Owned<PKCS12, PKCS12_free> noMac(PKCS12_init(NID_pkcs7_data), PKCS12_free);
    check(safes && noMac && PKCS12_pack_authsafes(noMac.get(), safes) == 1, "Synthetic no-MAC P12");
    sk_PKCS7_pop_free(safes, PKCS7_free);
    rejected("missing P12 authentication MAC",
             [&] { inspectSigningCertificate(encodeP12(noMac.get()), password); });
    const auto denied = qEnvironmentVariable("TATSU_DENIED_SAVE_DIR");
    if (!denied.isEmpty())
    {
        const auto existing = fileHash(denied + "/existing.pdf");
        rejected(
            "actual NTFS write denial", [&]
            { exportSignedPdf(document, key, password, {}, denied + "/certificate-denied.pdf"); });
        check(!QFileInfo::exists(denied + "/certificate-denied.pdf") &&
                  fileHash(denied + "/existing.pdf") == existing,
              "NTFS rejection preserves destination originals");
    }
    check(QDir(output).entryList({".pdf-tatsujin-save-*"}, QDir::Dirs | QDir::Hidden).isEmpty(),
          "All failed candidates cleaned");
    check(encodePdf(document) == before &&
              fileHash(fixtures + "/D07.pdf") == document.getSourceDataHash(),
          "Source unchanged after all failures");
    return {{"rejections", errors},
            {"atomic_nonreplacement", true},
            {"candidate_cleanup", true},
            {"actual_volume_full", "未実行"}};
}

QJsonObject testCertificateSigningOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.history = {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6})};
    window.doc.saved = -1;
    window.doc.putSignature(0, "証明書署名前の日本語署名", {30, 30}, 10, Qt::black);
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
          "Actual Japanese/English OCR worker finishes");
    check(error.isEmpty(), error);
    const auto before = encodePdf(window.doc.pdf());
    const auto cursor = window.doc.cursor;
    writeCandidate(window.doc.pdf(), output + "/certificate-ocr-before.pdf");
    const auto key = makeKey(true, "OCR synthetic password");
    const auto path = output + "/certificate-ocr-signed.pdf";
    const auto hash =
        exportSignedPdf(window.doc.pdf(), key, "OCR synthetic password", "日英OCRを保持", path);
    check(encodePdf(window.doc.pdf()) == before && window.doc.cursor == cursor &&
              window.doc.dirty(),
          "Signing preserves original OCR and history");
    window.doc.open(path);
    window.refresh(true);
    check(!window.doc.readOnly.isEmpty() &&
              verifyCertificateSignatures(path, hash, {false, {}}).signatures.first().integrity ==
                  SignatureIntegrity::Unchanged,
          "OCR signed PDF is intact and read-only");
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Frozen Japanese and English OCR terms retained");
    auto search = window.findChild<SearchPanel*>("searchPanel");
    for (const auto& term : {QString("市民公園"), QString("coastal")})
    {
        window.canvas->setFocus();
        QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
        window.query->setText(term);
        QTest::keyClick(window.query, Qt::Key_Return);
        check(QTest::qWaitFor(
                  [&] {
                      return search->session()->complete() &&
                             !search->session()->matches().isEmpty();
                  },
                  15000),
              "Actual signed PDF search: " + term);
    }
    window.canvas->setZoom(.5);
    window.canvas->goToPage(0);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "Signed OCR page ready");
    const auto size = pageSize(window.doc.pdf().getCatalog()->getPage(0));
    const auto first = window.canvas->mapFromScene({1, 1});
    const auto last = window.canvas->mapFromScene({size.width() - 1, size.height() - 1});
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 60);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "Signed OCR text selection ready");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto text = QApplication::clipboard()->text();
    check(text.size() > 500 && text.contains("市民公園"), "Actual Qt copy after signing");
    check(signatures(window.doc.pdf(), 0).first().text == "証明書署名前の日本語署名",
          "Visible signature survives OCR and certificate signing");
    return {{"actual_bilingual_OCR_search_and_Qt_copy", true},
            {"copied_characters", text.size()},
            {"signed_hash", QString::fromLatin1(hash.toHex())},
            {"native_clipboard", "未実行"}};
}
} // namespace tatsu
