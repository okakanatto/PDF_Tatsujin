#include "certificate_tests.h"
#include "certificate_dialog.h"
#include "document.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool ok, const QString& message)
{
    if (!ok)
        fail(message);
}
QByteArray bytes(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read test file: " + path);
    return file.readAll();
}
QString integrityName(SignatureIntegrity value)
{
    switch (value)
    {
    case SignatureIntegrity::Unsigned:
        return "Unsigned";
    case SignatureIntegrity::Unchanged:
        return "Unchanged";
    case SignatureIntegrity::Invalid:
        return "Invalid";
    default:
        return "Unsupported";
    }
}
QString chainName(CertificateChain value)
{
    switch (value)
    {
    case CertificateChain::LocalTrusted:
        return "LocalTrusted";
    case CertificateChain::Untrusted:
        return "Untrusted";
    case CertificateChain::Expired:
        return "Expired";
    case CertificateChain::NotYetValid:
        return "NotYetValid";
    case CertificateChain::Invalid:
        return "Invalid";
    default:
        return "NotChecked";
    }
}
QJsonObject rowJson(const CertificateSignature& row)
{
    return {{"field", row.field},
            {"integrity", integrityName(row.integrity)},
            {"chain", chainName(row.chain)},
            {"entire_file", row.entireFile},
            {"unsigned_tail", row.unsignedTail},
            {"detail", row.detail},
            {"certificate_subject", row.certificate.subject},
            {"certificate_sha256", row.certificate.fingerprint}};
}
CertificateTrust trust(const QString& fixtures)
{
    const auto folder = fixtures + "/certificate-signatures/";
    const auto manifest = QJsonDocument::fromJson(bytes(folder + "manifest.json")).object();
    const auto der = bytes(folder + "test-ca.der");
    check(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex() ==
              manifest["test_ca_sha256"].toString().toLatin1(),
          "Frozen test CA unchanged");
    return {false, {der}};
}
template <typename F> QString rejects(F callback)
{
    try
    {
        callback();
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
    fail("Certificate verification unexpectedly succeeded");
}
} // namespace
QJsonObject testCertificateCases(const QString& fixtures, const QString& output)
{
    const auto folder = fixtures + "/certificate-signatures/";
    const auto manifest = QJsonDocument::fromJson(bytes(folder + "manifest.json")).object();
    const auto testTrust = trust(fixtures);
    QJsonArray cases;
    for (const auto& item : manifest["cases"].toArray())
    {
        const auto expected = item.toObject();
        const auto file = expected["file"].toString();
        const auto path = folder + file;
        const auto hash = fileHash(path);
        check(hash.toHex() == expected["sha256"].toString().toLatin1(),
              "Frozen case unchanged: " + file);
        const auto actual = verifyCertificateSignatures(path, hash, testTrust);
        check(actual.signatures.size() == (file == "double.pdf" ? 2 : 1),
              "Signature count: " + file);
        const auto& row = actual.signatures.last();
        const auto details = rowJson(row);
        check(integrityName(row.integrity) == expected["integrity"].toString(),
              "Integrity " + file + ": " + row.detail);
        check(chainName(row.chain) == expected["chain"].toString(),
              "Certificate chain " + file + ": " + certificateChainText(row.chain));
        if (row.integrity == SignatureIntegrity::Unchanged)
            check(row.entireFile == expected["entire_file"].toBool(),
                  "Signature coverage: " + file);
        if (file == "double.pdf")
            check(actual.signatures.first().integrity == SignatureIntegrity::Unchanged &&
                      !actual.signatures.first().entireFile &&
                      actual.signatures.first().unsignedTail > 0,
                  "First signature remains valid for earlier revision only");
        check(fileHash(path) == hash, "Input PDF unchanged: " + file);
        cases << QJsonObject{{"file", file}, {"details", details}};
    }
    const auto valid = folder + "valid.pdf";
    const auto untrusted = verifyCertificateSignatures(valid, fileHash(valid), {false, {}});
    check(untrusted.signatures.first().integrity == SignatureIntegrity::Unchanged &&
              untrusted.signatures.first().chain == CertificateChain::Untrusted,
          "Cryptographic match is separate from absent certificate trust");
    const auto legacy = verifyCertificateSignatures(
        fixtures + "/D08-signed.pdf", fileHash(fixtures + "/D08-signed.pdf"), {false, {}});
    check(!legacy.signatures.isEmpty() &&
              legacy.signatures.first().integrity == SignatureIntegrity::Unchanged &&
              legacy.signatures.first().chain == CertificateChain::Untrusted,
          "Original frozen signed corpus verified");
    QFile record(output + "/certificate-cases.json");
    check(record.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Fresh certificate result");
    record.write(
        QJsonDocument(QJsonObject{{"cases", cases},
                                  {"test_ca_installed_in_OS", false},
                                  {"legacy_signed_fixture", rowJson(legacy.signatures.first())},
                                  {"without_test_CA", rowJson(untrusted.signatures.first())}})
            .toJson());
    return {{"synthetic_cases", cases.size()},
            {"legacy_fixture_verified", true},
            {"private_keys_retained", false}};
}
QJsonObject testCertificateGuards(const QString& fixtures, const QString& output)
{
    const auto source = fixtures + "/certificate-signatures/valid.pdf";
    const auto hash = fileHash(source);
    const auto testTrust = trust(fixtures);
    QJsonArray errors;
    auto rejection = [&](const QString& label, auto function)
    { errors << QJsonObject{{"condition", label}, {"error", rejects(function)}}; };
    rejection("cancel before read",
              [&] { verifyCertificateSignatures(source, hash, testTrust, [] { return true; }); });
    rejection("wrong source hash",
              [&] { verifyCertificateSignatures(source, QByteArray(32, 'x'), testTrust); });
    rejection("missing hash", [&] { verifyCertificateSignatures(source, {}, testTrust); });
    rejection("missing file",
              [&] { verifyCertificateSignatures(output + "/nonexistent.pdf", hash, testTrust); });
    rejection("encrypted file",
              [&]
              {
                  const auto path = fixtures + "/D08-encrypted.pdf";
                  verifyCertificateSignatures(path, fileHash(path), testTrust);
              });
    int checkpoints = 0;
    verifyCertificateSignatures(source, hash, testTrust,
                                [&]
                                {
                                    ++checkpoints;
                                    return false;
                                });
    int late = 0;
    rejection("cancel near completion",
              [&]
              {
                  verifyCertificateSignatures(source, hash, testTrust,
                                              [&] { return ++late >= checkpoints - 1; });
              });
    const auto copy = output + "/certificate-changed-input.pdf";
    check(QFile::copy(source, copy), "Own changing-input copy");
    bool changed = false;
    int updateChecks = 0;
    rejection("file changes during verification",
              [&]
              {
                  verifyCertificateSignatures(copy, hash, testTrust,
                                              [&]
                                              {
                                                  if (++updateChecks >= 4 && !changed)
                                                  {
                                                      QFile file(copy);
                                                      check(file.open(QIODevice::Append),
                                                            "Modify own fixture copy");
                                                      file.write("\n% external update\n");
                                                      changed = true;
                                                  }
                                                  return false;
                                              });
              });
    Document original;
    original.open(source);
    const auto before = encodePdf(original.pdf());
    check(!original.readOnly.isEmpty(), "Signed document remains read-only");
    rejection("signed document editing blocked",
              [&] { original.putSignature(0, "禁止する編集", {50, 50}, 12, Qt::black); });
    verifyCertificateSignatures(source, hash, testTrust);
    check(encodePdf(original.pdf()) == before && original.cursor == 0 &&
              original.history.size() == 1 && fileHash(source) == hash &&
              !original.readOnly.isEmpty(),
          "Verification preserves document, history and editing restriction");
    return {{"rejections", errors},
            {"cancellation_checkpoints", checkpoints},
            {"source_and_history_unchanged", true}};
}
QJsonObject testCertificateUi(const QString& fixtures, const QString& output)
{
    const auto source = fixtures + "/certificate-signatures/valid.pdf";
    Window window;
    auto action = window.findChild<QAction*>("verifyDocumentCertificates");
    check(action && !action->isEnabled(), "Verification unavailable without PDF");
    window.openFile(source);
    window.show();
    check(action->isEnabled() && !window.signatureAction->isEnabled() &&
              !window.ocrAction->isEnabled(),
          "Signed PDF allows verification while mutation stays disabled");
    bool opened = false, finished = false;
    QString callbackError;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(
        &poll, &QTimer::timeout, &window,
        [&]
        {
            auto dialog = dynamic_cast<CertificateDialog*>(
                window.findChild<QDialog*>("certificateVerificationDialog"));
            if (!dialog)
                return;
            auto start = dialog->findChild<QPushButton*>("verifyCertificateSignatures");
            if (!opened)
            {
                opened = true;
                start->setFocus();
                QTest::keyClick(start, Qt::Key_Space);
            }
            if (!dialog->verification())
                return;
            const auto result = dialog->verification();
            try
            {
                check(result->signatures.size() == 1 &&
                          result->signatures.first().integrity == SignatureIntegrity::Unchanged &&
                          result->signatures.first().chain == CertificateChain::Untrusted,
                      "Windows roots do not trust synthetic CA");
                auto table = dialog->findChild<QTableWidget*>("certificateSignatures");
                auto text = dialog->findChild<QPlainTextEdit*>("certificateDetails");
                check(table->columnCount() == 4 && text->toPlainText().contains("失効状態") &&
                          text->toPlainText().contains("Synthetic PDF Signer"),
                      "Separated states and plain certificate identity");
                text->setFocus();
                QTest::keyClick(text, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
                check(QApplication::clipboard()->text().contains(
                          result->signatures.first().certificate.fingerprint),
                      "Actual Qt copy of certificate fingerprint");
                check(dialog->grab().save(output + "/certificate-verification.png"),
                      "Real offscreen verification screenshot");
                finished = true;
            }
            catch (const std::exception& error)
            {
                callbackError = QString::fromUtf8(error.what());
            }
            poll.stop();
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
    poll.start();
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &window,
                     [&]
                     {
                         if (auto dialog =
                                 window.findChild<QDialog*>("certificateVerificationDialog"))
                             dialog->reject();
                     });
    watchdog.start(30000);
    action->trigger();
    check(callbackError.isEmpty(), callbackError);
    check(opened && finished && fileHash(source) == window.doc.sourceHash,
          "Actual menu/dialog flow and unchanged original");
    CertificateDialog cancelledDialog(source, fileHash(source));
    cancelledDialog.show();
    QTest::mouseClick(cancelledDialog.findChild<QPushButton*>("verifyCertificateSignatures"),
                      Qt::LeftButton);
    QTest::keyClick(&cancelledDialog, Qt::Key_Escape);
    check(QTest::qWaitFor([&] { return !cancelledDialog.isVisible(); }, 30000) &&
              !cancelledDialog.verification(),
          "Escape cancels running verification and closes after job ends");
    window.openFile(fixtures + "/D02.pdf");
    window.doc.putSignature(0, "未保存の状態", {70, 100}, 12, Qt::black);
    window.refresh();
    check(!action->isEnabled(), "Unsaved state cannot be mistaken for verified disk bytes");
    window.doc.saved = window.doc.cursor;
    window.hide();
    return {{"menu_flow", true},
            {"keyboard_and_Qt_copy", true},
            {"running_cancel", true},
            {"native_clipboard_IME_and_OS_DPI", "未実行"}};
}
} // namespace tatsu
