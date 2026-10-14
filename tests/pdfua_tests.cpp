#include "pdfua_tests.h"
#include "pdf_validation_dialog.h"
#include "settings_guard.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool condition, const QString& reason)
{
    if (!condition)
        fail(reason);
}
QJsonObject fixed(const QString& fixtures)
{
    QFile file(fixtures + "/pdfua-validation/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read pre-implementation PDF/UA truth");
    const auto bytes = file.readAll();
    check(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() ==
              "d1ebf99ed5edfb6d81c440fb9d0a1c38c8ed8cb06444a3532d2e8ffde2507efc",
          "Frozen PDF/UA criterion bytes");
    const auto criterion = QJsonDocument::fromJson(bytes).object();
    for (const auto& value : criterion["inputs"].toArray())
    {
        const auto row = value.toObject();
        check(fileHash(fixtures + "/pdfua-validation/" + row["file"].toString()).toHex() ==
                  row["sha256"].toString().toLatin1(),
              "Immutable upstream PDF/UA PDF SHA");
    }
    return criterion;
}
QString javaPath()
{
    return qEnvironmentVariable("TATSU_PDFA_JAVA");
}
QString jarPath()
{
    return qEnvironmentVariable("TATSU_PDFA_JAR");
}
void checkScope(const QJsonObject& result)
{
    check(result["validation_scope"].toString() == "machine-verifiable" &&
              result["full_conformance"].toString() == "not-determined" &&
              result.contains("machine_compliant") && !result.contains("compliant"),
          "Machine result never exports an unqualified full-conformance flag");
}
} // namespace
QJsonObject testPdfuaEngine(const QString& fixtures, const QString& output)
{
    const auto criterion = fixed(fixtures);
    QJsonArray results;
    for (const auto& value : criterion["inputs"].toArray())
    {
        const auto row = value.toObject();
        Document document;
        document.open(fixtures + "/pdfua-validation/" + row["file"].toString());
        const auto before = encodePdf(document.pdf());
        const auto result = validatePdfConformance(currentPdfValidationInput(document), javaPath(),
                                                   jarPath(), "ua1");
        check(result.checksPassed == (row["upstream_expected"].toString() == "pass") &&
                  result.engineVersion == "1.30.3" &&
                  result.snapshotHash == row["sha256"].toString(),
              "Real product verifier agrees with preselected upstream truth and original bytes");
        const auto json = pdfValidationResultJson(result);
        checkScope(json);
        exportPdfValidationResult(result, output + "/pdfua-" + row["case"].toString() + ".json");
        check(encodePdf(document.pdf()) == before && !document.dirty() && document.cursor == 0 &&
                  fileHash(document.source) == document.sourceHash,
              "PDF/UA machine verification never changes original or document history");
        results.append(json);
    }
    check(results.size() == 4, "All four frozen PDF/UA cases executed");
    return {{"four_real_engine_results", results},
            {"human_checkpoints", "未評価"},
            {"full_PDF_UA_conformance", "未判定"},
            {"original_bytes_and_history_retained", true}};
}
QJsonObject testPdfuaFailures(const QString& fixtures, const QString& output)
{
    fixed(fixtures);
    Document document;
    document.open(fixtures + "/pdfua-validation/version-pass.pdf");
    document.putSignature(0, "検査前の署名を保持", {80, 120}, 12, Qt::black);
    const auto before = encodePdf(document.pdf());
    const auto cursor = document.cursor;
    QFile snapshot(output + "/pdfua-unsaved-candidate.pdf");
    check(snapshot.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
              snapshot.write(before) == before.size(),
          "Preserve exact independently verifiable UA candidate");
    snapshot.close();
    const auto candidate =
        validatePdfConformance(currentPdfValidationInput(document), javaPath(), jarPath(), "ua1");
    check(candidate.snapshotHash ==
                  QString::fromLatin1(
                      QCryptographicHash::hash(before, QCryptographicHash::Sha256).toHex()) &&
              candidate.snapshotHash != QString::fromLatin1(document.sourceHash.toHex()),
          "Unsaved UA candidate gets its own hash instead of original result");
    checkScope(pdfValidationResultJson(candidate));
    const QByteArray complete =
        R"xml(<report><releaseDetails id="core" version="1.30.3"/><jobs><job><validationReport jobEndStatus="normal" profileName="PDF/UA-1 validation profile" isCompliant="true"><details failedRules="0" failedChecks="0"/></validationReport></job></jobs><batchSummary totalJobs="1" failedToParse="0" encrypted="0" outOfMemory="0" veraExceptions="0"><validationReports compliant="1" nonCompliant="0" failedJobs="0"/></batchSummary></report>)xml";
    checkScope(pdfValidationResultJson(parsePdfValidationReport(complete, "ua1", 0)));
    int refused = 0;
    const auto reject = [&](auto action)
    {
        bool rejected = false;
        try
        {
            action();
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        check(rejected, "Invalid UA request must remain unqualified");
        ++refused;
    };
    reject([&] { parsePdfValidationReport(complete, "2u", 0); });
    reject([&] { parsePdfValidationReport(complete, "ua2", 0); });
    reject([&] { parsePdfValidationReport(complete, "ua1", 1); });
    reject([&] { parsePdfValidationReport(complete.left(complete.size() - 10), "ua1", 0); });
    reject(
        [&] {
            parsePdfValidationReport("<!DOCTYPE report [<!ENTITY fake 'x'>]>" + complete, "ua1", 0);
        });
    reject(
        [&]
        {
            validatePdfConformance(currentPdfValidationInput(document), "missing-java.exe",
                                   jarPath(), "ua1");
        });
    reject(
        [&]
        {
            validatePdfConformance(currentPdfValidationInput(document), javaPath(), jarPath(),
                                   "ua1", [] { return true; });
        });
    const auto existing = output + "/pdfua-existing.json";
    exportPdfValidationResult(candidate, existing);
    const auto retained = fileHash(existing);
    reject([&] { exportPdfValidationResult(candidate, existing); });
    check(fileHash(existing) == retained && encodePdf(document.pdf()) == before &&
              document.cursor == cursor,
          "UA rejection preserves prior result, unsaved signature and Undo");
    document.undo();
    check(signatures(document.pdf(), 0).isEmpty(), "Prior signature Undo remains available");
    return {{"refused_cases", refused},
            {"machine_scope_and_candidate_hash", true},
            {"source_unsaved_signature_and_Undo_retained", true},
            {"human_checkpoints", "未評価"}};
}
static QJsonObject pdfuaUi(const QString& fixtures, const QString& output, bool expectedPass)
{
    fixed(fixtures);
    test::SettingsGuard settings({"pdfa/java", "pdfa/jar"});
    Window window;
    window.doc.open(fixtures + (expectedPass ? "/pdfua-validation/version-pass.pdf"
                                             : "/pdfua-validation/version-fail.pdf"));
    window.refresh(true);
    window.show();
    const auto before = encodePdf(window.doc.pdf());
    QString error;
    QTimer automation;
    QObject::connect(
        &automation, &QTimer::timeout, &window,
        [&]
        {
            auto dialog = dynamic_cast<PdfValidationDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            automation.stop();
            try
            {
                auto profile = dialog->findChild<QComboBox*>("pdfValidationProfile");
                profile->setFocus();
                QTest::keyClick(profile, Qt::Key_End);
                check(profile->currentData().toString() == "ua1",
                      "Actual keyboard UA machine-profile selection");
                auto start = dialog->findChild<QPushButton*>("pdfValidationStart");
                auto copy = dialog->findChild<QPushButton*>("pdfValidationCopy");
                auto save = dialog->findChild<QPushButton*>("pdfValidationSave");
                auto message = dialog->findChild<QLabel*>("pdfValidationMessage");
                QTest::mouseClick(start, Qt::LeftButton);
                check(QTest::qWaitFor([&] { return start->isEnabled(); }, 30000) &&
                          copy->isEnabled() && save->isEnabled(),
                      "Real UA UI engine completion");
                check(message->text().contains(expectedPass ? "機械検査に適合"
                                                            : "機械検査で不適合") &&
                          (!expectedPass || message->text().contains("全体の適合は未判定")),
                      "UA UI never calls machine success full conformance");
                QTest::mouseClick(copy, Qt::LeftButton);
                check(QApplication::clipboard()->text().contains("人による確認") &&
                          QApplication::clipboard()->text().contains("全体の適合は未判定"),
                      "Copied UA result preserves scope");
                const auto path = output + (expectedPass ? "/pdfua-ui-result.json"
                                                         : "/pdfua-ui-fail-result.json");
                dialog->findChild<QLineEdit*>("pdfValidationOutput")->setText(path);
                QTest::mouseClick(save, Qt::LeftButton);
                QFile saved(path);
                check(saved.open(QIODevice::ReadOnly), "Real UA UI result saved");
                const auto savedResult = QJsonDocument::fromJson(saved.readAll()).object();
                checkScope(savedResult);
                check(savedResult["machine_compliant"].toBool() == expectedPass,
                      "Actual saved UI result follows frozen positive and negative truth");
                saved.close();
                check(dialog->grab().save(output +
                                          (expectedPass ? "/pdfua-ui.png" : "/pdfua-ui-fail.png")),
                      "Actual UA machine-result screen");
                dialog->resize(720, 480);
                QTest::qWait(30);
                check(dialog->size() == QSize(720, 480),
                      "Validation dialog keeps its supported minimum size");
                for (auto control : {static_cast<QWidget*>(start), static_cast<QWidget*>(copy),
                                     static_cast<QWidget*>(save), static_cast<QWidget*>(profile),
                                     static_cast<QWidget*>(message)})
                    check(control->isVisible() &&
                              dialog->rect().contains(
                                  QRect(control->mapTo(dialog, QPoint()), control->size())),
                          "Machine validation controls remain reachable at 720x480");
                check(dialog->grab().save(output + (expectedPass ? "/pdfua-ui-compact.png"
                                                                 : "/pdfua-ui-fail-compact.png")),
                      "Actual compact machine-result screen");
                QTest::keyClick(profile, Qt::Key_Home);
                check(!copy->isEnabled() && !save->isEnabled(),
                      "Changing format invalidates old UA result");
                QTest::keyClick(profile, Qt::Key_End);
                QTest::mouseClick(start, Qt::LeftButton);
                check(QTest::qWaitFor([&] { return !start->isEnabled(); }, 1000),
                      "Actual UA UI start enters busy state");
                QTest::mouseClick(dialog->findChild<QPushButton*>("pdfValidationClose"),
                                  Qt::LeftButton);
                check(QTest::qWaitFor([&] { return start->isEnabled(); }, 15000) &&
                          !copy->isEnabled() && !save->isEnabled(),
                      "UA UI cancellation publishes no result");
            }
            catch (const std::exception& failure)
            {
                error = QString::fromUtf8(failure.what());
            }
            dialog->reject();
        });
    automation.start(10);
    auto action = window.findChild<QAction*>("verifyPdfDocument");
    check(action, "Real document menu has PDF validation action");
    action->trigger();
    check(error.isEmpty(), error);
    check(encodePdf(window.doc.pdf()) == before && !window.doc.dirty(),
          "Real UA UI retains source state");
    return {{"actual_keyboard_verify_copy_export_cancel", true},
            {"unqualified_full_conformance_not_displayed", true},
            {"native_GUI_OS_clipboard_screenreader", "未実行"},
            {"human_checkpoints", "未評価"}};
}
QJsonObject testPdfuaUi(const QString& fixtures, const QString& output)
{
    return {{"machine_pass_UI", pdfuaUi(fixtures, output, true)},
            {"machine_fail_UI", pdfuaUi(fixtures, output, false)}};
}
} // namespace tatsu
