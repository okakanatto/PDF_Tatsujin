#include "pdfa_tests.h"
#include "owned_process.h"
#include "pdfa_dialog.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool condition, const QString& why)
{
    if (!condition)
        fail(why);
}
QJsonObject fixedPdfa(const QString& fixtures)
{
    QFile file(fixtures + "/pdfa-validation/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen PDF/A criteria");
    const auto data = file.readAll();
    check(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() ==
              "f100bd83cc754dfa2ebfc8d7cf399d90082c48309d7e20552a4f123fae279397",
          "PDF/A criteria fixed before implementation");
    const auto result = QJsonDocument::fromJson(data).object();
    for (const auto& entry : result["files"].toArray())
    {
        const auto row = entry.toObject();
        check(fileHash(fixtures + "/pdfa-validation/" + row["file"].toString()).toHex() ==
                  row["sha256"].toString().toLatin1(),
              "Frozen PDF/A file SHA");
    }
    return result;
}
QString javaPath()
{
    return qEnvironmentVariable("TATSU_PDFA_JAVA");
}
QString jarPath()
{
    return qEnvironmentVariable("TATSU_PDFA_JAR");
}
} // namespace
QJsonObject testPdfaEngine(const QString& fixtures, const QString& output)
{
    const auto criterion = fixedPdfa(fixtures);
    const auto version = runOwnedProcess(javaPath(), {"-version"}, output, 15000);
    QFile diagnostic(output + "/pdfa-java-version.txt");
    check(diagnostic.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Java diagnostic output");
    check(diagnostic.write(version.output) == version.output.size(), "Java diagnostic written");
    diagnostic.close();
    check(version.exitCode == 0, "Java runtime starts in the test identity");
    const auto probe = runOwnedProcess(
        javaPath(),
        {"-Xmx512m", "-Djava.awt.headless=true", "-Djava.io.tmpdir=" + output,
         "-Duser.home=" + output, "-jar", jarPath(), "--format", "xml", "--flavour", "1b",
         "--maxfailuresdisplayed", "1", fixtures + "/pdfa-validation/1b-pass.pdf"},
        output, 120000, {}, 4 * 1024 * 1024);
    QFile engineDiagnostic(output + "/pdfa-engine-probe.txt");
    check(engineDiagnostic.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          "PDF/A engine diagnostic output");
    check(engineDiagnostic.write(probe.output) == probe.output.size(),
          "PDF/A engine diagnostic written");
    engineDiagnostic.close();
    check(probe.exitCode == 0 && !probe.outputTruncated,
          "PDF/A engine starts in the test identity");
    QJsonArray results;
    for (const auto& value : criterion["files"].toArray())
    {
        const auto row = value.toObject();
        if (!row.contains("expected_compliant"))
            continue;
        Document document;
        document.open(fixtures + "/pdfa-validation/" + row["file"].toString());
        const auto original = encodePdf(document.pdf());
        const auto input = currentPdfaInput(document);
        check(!input.source.isEmpty(), "Unedited PDF validates original bytes without re-save");
        const auto result = validatePdfa(input, javaPath(), jarPath(), row["profile"].toString());
        check(result.compliant == row["expected_compliant"].toBool() &&
                  result.engineVersion == "1.30.3" &&
                  result.snapshotHash == row["sha256"].toString(),
              "Real verifier agrees with frozen PDF/A truth and original bytes");
        check(encodePdf(document.pdf()) == original && !document.dirty() &&
                  fileHash(document.source) == document.sourceHash,
              "PDF/A check preserves source and document");
        exportPdfaResult(result, output + "/pdfa-" + QFileInfo(document.source).completeBaseName() +
                                     ".json");
        results.append(pdfaResultJson(result));
    }
    check(results.size() == 5, "Five real PDF/A cases completed");
    return {{"five_fixed_real_engine_results", results},
            {"original_bytes_without_reencoding", true},
            {"no_original_change", true}};
}
QJsonObject testPdfaFailures(const QString& fixtures, const QString& output)
{
    fixedPdfa(fixtures);
    Document document;
    document.open(fixtures + "/pdfa-validation/2u-pass.pdf");
    const auto valid = validatePdfa(currentPdfaInput(document), javaPath(), jarPath(), "2u");
    document.putSignature(0, "未保存状態を保持", {80, 120}, 12, Qt::black);
    const auto original = encodePdf(document.pdf());
    const auto cursor = document.cursor;
    const auto draft = currentPdfaInput(document);
    check(draft.source.isEmpty(), "Unsaved edits validate their own candidate, not old source");
    const auto candidate = validatePdfa(draft, javaPath(), jarPath(), "2u");
    check(candidate.snapshotHash ==
                  QString::fromLatin1(
                      QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex()) &&
              candidate.snapshotHash != QString::fromLatin1(document.sourceHash.toHex()),
          "Actual unsaved candidate validated with its own SHA, not original acceptance");
    int refused = 0;
    const auto refuse = [&](auto operation)
    {
        bool rejected = false;
        try
        {
            operation();
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        check(rejected, "Incomplete PDF/A request refuses without success");
        ++refused;
    };
    refuse([&] { validatePdfa(draft, "missing-java.exe", jarPath(), "2u"); });
    refuse([&] { validatePdfa(draft, javaPath(), "missing.jar", "2u"); });
    refuse([&] { validatePdfa(draft, javaPath(), jarPath(), "other"); });
    refuse([&] { validatePdfa(draft, javaPath(), jarPath(), "2u", [] { return true; }); });
    Document encrypted;
    encrypted.open(fixtures + "/pdfa-validation/copy-restricted.pdf");
    refuse([&] { validatePdfa(currentPdfaInput(encrypted), javaPath(), jarPath(), "2u"); });
    const auto target = output + "/pdfa-existing.json";
    QFile sentinel(target);
    check(sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Owned PDF/A output sentinel");
    sentinel.write("retain existing result");
    sentinel.close();
    const auto savedHash = fileHash(target);
    refuse([&] { exportPdfaResult(valid, target); });
    refuse([&] { exportPdfaResult(valid, output + "/pdfa-wrong.txt"); });
    const auto source = output + "/pdfa-external-change.pdf";
    check(QFile::copy(fixtures + "/pdfa-validation/2u-pass.pdf", source), "Owned change test copy");
    Document changed;
    changed.open(source);
    QFile altered(source);
    check(altered.open(QIODevice::Append), "Owned external writer");
    altered.write("\n% changed\n");
    altered.close();
    refuse([&] { validatePdfa(currentPdfaInput(changed), javaPath(), jarPath(), "2u"); });
    const QByteArray complete =
        R"xml(<report><buildInformation><releaseDetails id="core" version="1.30.3"/></buildInformation><jobs><job><validationReport jobEndStatus="normal" profileName="PDF/A-2u validation profile" isCompliant="true"><details failedRules="0" failedChecks="0"/></validationReport></job></jobs><batchSummary totalJobs="1" failedToParse="0" encrypted="0" outOfMemory="0" veraExceptions="0"><validationReports compliant="1" nonCompliant="0" failedJobs="0"/></batchSummary></report>)xml";
    check(parsePdfaReport(complete, "2u", 0).compliant, "Complete protocol control");
    const auto truncated =
        runOwnedProcess(javaPath(), {"-jar", jarPath(), "--help"}, output, 30000, {}, 1024);
    check(truncated.exitCode == 0 && truncated.output.size() == 1024 && truncated.outputTruncated,
          "Real external output limit reports truncation rather than completion");
    refuse([&] { parsePdfaReport(complete.left(complete.size() - 10), "2u", 0); });
    refuse([&] { parsePdfaReport(complete, "1b", 0); });
    refuse([&] { parsePdfaReport(complete, "2u", 2); });
    for (const auto& replacement :
         {std::pair{QByteArray("totalJobs=\"1\""), QByteArray("totalJobs=\"2\"")},
          std::pair{QByteArray("failedToParse=\"0\""), QByteArray("failedToParse=\"1\"")},
          std::pair{QByteArray("isCompliant=\"true\""), QByteArray("isCompliant=\"false\"")}})
    {
        auto invalid = complete;
        invalid.replace(replacement.first, replacement.second);
        refuse([&] { parsePdfaReport(invalid, "2u", 0); });
    }
    refuse([&] { parsePdfaReport("<!DOCTYPE report [<!ENTITY fake 'x'>]>" + complete, "2u", 0); });
    check(fileHash(target) == savedHash && encodePdf(document.pdf()) == original &&
              document.cursor == cursor && document.dirty(),
          "PDF/A failure retains existing result, unsaved signature and Undo");
    return {{"refused_cases", refused},
            {"partial_wrong_profile_or_failed_protocol_rejected", true},
            {"encrypted_no_implicit_decryption", true},
            {"unsaved_state_retained", true}};
}
QJsonObject testPdfaUi(const QString& fixtures, const QString& output)
{
    fixedPdfa(fixtures);
    struct RestoreSettings
    {
        QMap<QString, QVariant> values;
        QSet<QString> present;
        RestoreSettings()
        {
            QSettings s;
            for (const auto& key : {QString("pdfa/java"), QString("pdfa/jar")})
            {
                if (s.contains(key))
                    present.insert(key);
                values[key] = s.value(key);
            }
        }
        ~RestoreSettings()
        {
            QSettings s;
            for (auto i = values.cbegin(); i != values.cend(); ++i)
            {
                if (present.contains(i.key()))
                    s.setValue(i.key(), i.value());
                else
                    s.remove(i.key());
            }
        }
    } restore;
    Window window;
    window.show();
    window.openFile(fixtures + "/pdfa-validation/1b-pass.pdf");
    const auto original = encodePdf(window.doc.pdf()), sourceHash = fileHash(window.doc.source);
    bool operated = false;
    QString error;
    QTimer automation;
    QObject::connect(
        &automation, &QTimer::timeout, &window,
        [&]
        {
            auto dialog = dynamic_cast<PdfaDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            automation.stop();
            try
            {
                auto profile = dialog->findChild<QComboBox*>("pdfaProfile");
                profile->setFocus();
                QTest::keyClick(profile, Qt::Key_End);
                auto start = dialog->findChild<QPushButton*>("pdfaStart");
                auto copy = dialog->findChild<QPushButton*>("pdfaCopy");
                auto save = dialog->findChild<QPushButton*>("pdfaSave");
                QTest::mouseClick(start, Qt::LeftButton);
                check(QTest::qWaitFor([&] { return start->isEnabled(); }, 30000) &&
                          copy->isEnabled() && save->isEnabled(),
                      "Real PDF/A UI engine completion");
                auto message = dialog->findChild<QLabel*>("pdfaMessage");
                check(message->text().contains("適合を確認"), "Real PDF/A UI compliant result");
                QTest::mouseClick(copy, Qt::LeftButton);
                check(QApplication::clipboard()->text().contains("判定：適合") &&
                          QApplication::clipboard()->text().contains("veraPDF 1.30.3"),
                      "PDF/A actual result copy");
                const auto path = output + "/pdfa-ui-result.json";
                dialog->findChild<QLineEdit*>("pdfaOutput")->setText(path);
                QTest::mouseClick(save, Qt::LeftButton);
                check(QFileInfo::exists(path), "PDF/A real UI save");
                const auto hash = fileHash(path);
                dialog->grab().save(output + "/pdfa-ui.png");
                QTest::mouseClick(save, Qt::LeftButton);
                check(fileHash(path) == hash && message->text().contains("上書きしません"),
                      "PDF/A UI existing result preserved");
                dialog->grab().save(output + "/pdfa-ui-existing-rejected.png");
                QTest::keyClick(profile, Qt::Key_Home);
                check(
                    !copy->isEnabled() && !save->isEnabled() &&
                        dialog->findChild<QPlainTextEdit*>("pdfaDetails")->toPlainText().isEmpty(),
                    "Changing PDF/A profile invalidates old result");
                dialog->findChild<QLineEdit*>("pdfaJava")->setText("missing-java.exe");
                QTest::mouseClick(start, Qt::LeftButton);
                check(QTest::qWaitFor([&] { return start->isEnabled(); }, 15000) &&
                          !copy->isEnabled() && !save->isEnabled() &&
                          message->text().contains("未実行"),
                      "Missing engine never appears compliant");
                dialog->findChild<QLineEdit*>("pdfaJava")->setText(javaPath());
                QTest::mouseClick(start, Qt::LeftButton);
                check(QTest::qWaitFor([&] { return start->isEnabled(); }, 30000) &&
                          copy->isEnabled(),
                      "Fresh PDF/A result before stale document test");
                window.doc.putSignature(0, "古い結果を無効にする変更", {80, 250}, 12, Qt::black);
                QTest::mouseClick(copy, Qt::LeftButton);
                check(!copy->isEnabled() && !save->isEnabled() &&
                          dialog->findChild<QPlainTextEdit*>("pdfaDetails")
                              ->toPlainText()
                              .isEmpty() &&
                          message->text().contains("変更されました"),
                      "Changed document cannot reuse previous PDF/A compliance");
                window.doc.undo();
                operated = true;
                QTest::mouseClick(dialog->findChild<QPushButton*>("pdfaClose"), Qt::LeftButton);
            }
            catch (const std::exception& exception)
            {
                error = QString::fromUtf8(exception.what());
                dialog->reject();
            }
        });
    automation.start(25);
    auto action = window.findChild<QAction*>("verifyPdfaDocument");
    check(action && action->isEnabled(), "Real PDF/A menu entry");
    action->trigger();
    check(operated && error.isEmpty(), "PDF/A UI sequence: " + error);
    check(encodePdf(window.doc.pdf()) == original && fileHash(window.doc.source) == sourceHash &&
              !window.doc.dirty(),
          "PDF/A menu preserves original");
    window.doc.putSignature(0, "取消で保持する署名", {80, 200}, 12, Qt::black);
    const auto draft = encodePdf(window.doc.pdf());
    const auto cursor = window.doc.cursor;
    PdfaDialog cancelled(currentPdfaInput(window.doc), {}, &window);
    cancelled.show();
    QTest::mouseClick(cancelled.findChild<QPushButton*>("pdfaStart"), Qt::LeftButton);
    QTest::qWait(50);
    cancelled.reject();
    check(QTest::qWaitFor([&] { return !cancelled.isVisible(); }, 15000),
          "Owned real verifier UI cancellation");
    check(encodePdf(window.doc.pdf()) == draft && window.doc.cursor == cursor && window.doc.dirty(),
          "PDF/A cancel retains unsaved signature and Undo");
    return {{"actual_menu_engine_copy_new_save", true},
            {"profile_changes_invalidate", true},
            {"missing_engine_and_cancel_no_success", true},
            {"native_GUI_IME", "未実行"}};
}
} // namespace tatsu
