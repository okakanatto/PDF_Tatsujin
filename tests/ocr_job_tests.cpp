#include "ocr_job_tests.h"
#include "document.h"
#include "ocr_job.h"
#include "ocr_jobs.h"
#include "private_temp.h"
#include "window.h"
#include <QtTest/QTest>
#include <functional>
#include <windows.h>

QJsonObject testOcrWindowTeardown(const QString& fixtures, const QString&)
{
    const auto source = fixtures + "/D03.pdf";
    const auto original = tatsu::fileHash(source);
    auto window = std::make_unique<tatsu::Window>();
    window->openFile(source);
    window->doc.putSignature(0, "破棄時の署名", {50, 40}, 16, Qt::black);
    window->refresh();
    window->startOcr();
    const auto directory = window->work->path();
    if (!QTest::qWaitFor(
            [&] { return window->worker && window->progress->text().startsWith("OCR 1 /"); },
            90000))
        tatsu::fail("Worker did not reach partial progress before Window destruction");
    QPointer<QProcess> worker = window->worker;
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(worker->processId()));
    if (!process)
        tatsu::fail("Cannot observe owned OCR process teardown");
    const auto close = qScopeGuard([&] { CloseHandle(process); });
    int dialogs = 0;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout,
                     [&]
                     {
                         for (auto widget : QApplication::topLevelWidgets())
                             if (auto box = qobject_cast<QMessageBox*>(widget))
                             {
                                 ++dialogs;
                                 box->reject();
                             }
                     });
    dismiss.start(20);
    window.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (dialogs || worker || WaitForSingleObject(process, 0) != WAIT_OBJECT_0 ||
        QFileInfo::exists(directory) || tatsu::fileHash(source) != original)
        tatsu::fail("Window destruction leaked worker/job or invoked completion on destroyed UI");
    return {{"partial_progress_before_destruction", true},
            {"no_completion_dialog", true},
            {"worker_terminated_and_QObject_destroyed", true},
            {"owned_job_removed_and_source_unchanged", true}};
}

QJsonObject testOcrResultValidation(const QString& fixtures, const QString& output)
{
    using namespace tatsu;
    QTemporaryDir root(output + "/result-contract-XXXXXX");
    if (!root.isValid())
        fail("Cannot create result contract test root");
    Document document;
    document.open(fixtures + "/D03.pdf");
    const auto original = fileHash(document.source);
    document.putSignature(0, "結果異常でも保持する署名", {50, 40}, 16, Qt::black);
    auto job = OcrJob::prepare(document, {"jpn+eng", "1,3"}, root.path());
    const auto directory = job->path();
    const QJsonObject valid{
        {"language", "jpn+eng"},
        {"pages", QJsonArray{QJsonObject{{"page", 1}, {"status", "処理済み"}},
                             QJsonObject{{"page", 3}, {"status", "既存OCR保持"}}}}};
    auto write = [&](const QByteArray& bytes)
    {
        QFile file(job->filePath("report.json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            fail("Cannot write synthetic worker report");
    };
    auto report = [&](const QJsonObject& value) { write(QJsonDocument(value).toJson()); };
    QJsonArray rejected;
    auto reject = [&](const QString& name, const std::function<void()>& operation)
    {
        const auto before = encodePdf(document.pdf());
        const auto cursor = document.cursor, saved = document.saved;
        const auto revision = document.revision;
        const auto history = document.history.size();
        bool refused = false;
        try
        {
            operation();
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        if (!refused || encodePdf(document.pdf()) != before || document.cursor != cursor ||
            document.saved != saved || document.revision != revision ||
            document.history.size() != history || !document.dirty() ||
            fileHash(document.source) != original)
            fail("Result rejection changed document or history: " + name);
        rejected.append(name);
    };
    // Keep a valid PDF for malformed-report cases: rejection must come from the
    // report contract rather than an unrelated missing output file.
    writeCandidate(document.pdf(), job->filePath("result.pdf"));
    reject("missing report", [&] { job->readResult(document); });
    for (const auto& bytes : {QByteArray(), QByteArray("{"), QByteArray("[]"), QByteArray("{}")})
    {
        write(bytes);
        reject("invalid JSON/schema: " + QString::fromUtf8(bytes),
               [&] { job->readResult(document); });
    }
    auto invalid = valid;
    invalid["language"] = "eng";
    report(invalid);
    reject("wrong language", [&] { job->readResult(document); });
    const auto rows = valid["pages"].toArray();
    const QVector<QPair<QString, QJsonArray>> invalidPages{
        {"partial report", {rows[0]}},
        {"duplicate page", {rows[0], rows[0]}},
        {"reordered pages", {rows[1], rows[0]}},
        {"unexpected page", {rows[0], QJsonObject{{"page", 4}, {"status", "処理済み"}}}},
        {"fractional page", {QJsonObject{{"page", 1.5}, {"status", "処理済み"}}, rows[1]}},
        {"string page", {QJsonObject{{"page", "1"}, {"status", "処理済み"}}, rows[1]}},
        {"unknown status", {QJsonObject{{"page", 1}, {"status", "unknown"}}, rows[1]}},
        {"missing status", {QJsonObject{{"page", 1}}, rows[1]}},
        {"non-object row", {false, rows[1]}}};
    for (const auto& bad : invalidPages)
    {
        invalid = valid;
        invalid["pages"] = bad.second;
        report(invalid);
        reject(bad.first, [&] { job->readResult(document); });
    }
    report(valid);
    if (!QFile::remove(job->filePath("result.pdf")))
        fail("Cannot remove owned synthetic output for missing-PDF case");
    reject("missing result PDF", [&] { job->readResult(document); });
    QFile broken(job->filePath("result.pdf"));
    if (!broken.open(QIODevice::WriteOnly) || broken.write("broken PDF") != 10)
        fail("Cannot write synthetic broken result");
    broken.close();
    reject("invalid result PDF", [&] { job->readResult(document); });
    writeCandidate(readPdf(fixtures + "/D01.pdf"), job->filePath("result.pdf"));
    reject("wrong PDF page count", [&] { job->readResult(document); });
    writeCandidate(document.pdf(), job->filePath("result.pdf"));
    const auto accepted = job->readResult(document);
    if (!accepted.changed() || accepted.pages.size() != 2 || accepted.pages[1].page != 3 ||
        signatures(accepted.document, 0).size() != 1 || !document.dirty())
        fail("Complete synthetic protocol result rejected or signature lost");
    invalid = valid;
    invalid["pages"] = QJsonArray{QJsonObject{{"page", 1}, {"status", "文字未検出"}}, rows[1]};
    report(invalid);
    if (job->readResult(document).changed())
        fail("No-op OCR result incorrectly reports new text");
    document.rotate(0);
    reject("stale document revision", [&] { job->readResult(document); });
    reject("invalid request language",
           [&] { OcrJob::prepare(document, {"invalid", "1"}, root.path()); });
    reject("duplicate requested page",
           [&] { OcrJob::prepare(document, {"jpn", "1,1"}, root.path()); });
    QFile blocker(root.filePath("not-a-directory"));
    if (!blocker.open(QIODevice::WriteOnly))
        fail("Cannot prepare owned file as invalid temporary root");
    blocker.close();
    reject("temporary preparation failure",
           [&] { OcrJob::prepare(document, {"jpn", "1"}, blocker.fileName()); });
    if (!cleanAbandonedOcrJobs(root.path()).isEmpty() || !QFileInfo::exists(directory))
        fail("Startup reclaimed live prepared OCR job");
    job.reset();
    if (QFileInfo::exists(directory))
        fail("Result job did not release lock and remove owned files");
    document.undo();
    document.redo();
    if (signatures(document.pdf(), 0).size() != 1 || fileHash(document.source) != original)
        fail("Result failure damaged Undo or original");
    return {{"rejected_cases", rejected},
            {"complete_and_no_op_results_accepted", true},
            {"state_history_source_and_signature_preserved", true},
            {"live_lock_and_cleanup", true},
            {"scope", "Synthetic worker protocol boundary cases; real OCR is tested separately"}};
}

QJsonObject testOcrJobCleanup(const QString& output)
{
    QTemporaryDir root(output + "/cleanup-XXXXXX");
    if (!root.isValid())
        tatsu::fail("Cannot create cleanup test directory");
    auto make = [&](const QString& name, const QByteArray& marker)
    {
        const auto path = root.filePath("pdf-tatsujin-job-" + name);
        if (!QDir().mkpath(path))
            tatsu::fail("Cannot create cleanup case");
        QFile file(path + "/.tatsujin-owner");
        if (!file.open(QIODevice::WriteOnly) || file.write(marker) != marker.size())
            tatsu::fail("Cannot write ownership marker");
        return path;
    };
    const auto abandoned = make("abandoned", "PDFTatsujin job v1");
    const auto parentPath = make("parent", "PDFTatsujin job v1");
    const auto workerPath = make("worker", "PDFTatsujin job v1");
    const auto unrelated = make("unrelated", "Another application");
    QLockFile parent(parentPath + "/job.lock");
    if (!parent.tryLock())
        tatsu::fail("Cannot acquire real parent lock");
    auto worker = tatsu::lockOwnedOcrWorker(workerPath + "/input.pdf");
    if (!worker)
        tatsu::fail("Cannot acquire real worker lock");
    bool duplicateRejected = false;
    try
    {
        auto duplicate = tatsu::lockOwnedOcrWorker(workerPath + "/input.pdf");
    }
    catch (const std::exception&)
    {
        duplicateRejected = true;
    }
    const auto removed = tatsu::cleanAbandonedOcrJobs(root.path());
    if (removed != QStringList{"pdf-tatsujin-job-abandoned"} || QFileInfo::exists(abandoned) ||
        !QFileInfo::exists(parentPath) || !QFileInfo::exists(workerPath) ||
        !QFileInfo::exists(unrelated) || !duplicateRejected)
        tatsu::fail("Cleanup removed active or unrelated data");
    worker.reset();
    const auto completed = tatsu::cleanAbandonedOcrJobs(root.path());
    if (completed != QStringList{"pdf-tatsujin-job-worker"} || QFileInfo::exists(workerPath))
        tatsu::fail("Abandoned worker job was not removed after releasing its lock");
    return {{"abandoned_removed", true},
            {"active_parent_preserved", true},
            {"active_worker_preserved", true},
            {"unrelated_preserved", true},
            {"duplicate_worker_rejected", true},
            {"lock_released_cleanup", true},
            {"scope", "real Qt filesystem locks in isolated Windows test directories"}};
}
QJsonObject testLongWindowsPaths(const QString& fixtures, const QString& output)
{
    auto root = tatsu::privateTemporaryDirectory(output + "/long-paths-XXXXXX");
    if (!root->isValid())
        tatsu::fail("Cannot create owned long-path test root");
    auto directory = root->path();
    while (directory.size() < 350)
        directory += "/日本語の長い保存先-abcdefghijk";
    if (!QDir().mkpath(directory))
        tatsu::fail("Cannot create long-path parent");
    QString temporary;
    {
        auto work = tatsu::privateTemporaryDirectory(directory + "/private-XXXXXX");
        if (!work->isValid())
            tatsu::fail("Cannot create private directory beyond MAX_PATH");
        temporary = work->path();
        QFile file(work->filePath("日本語.txt"));
        const auto content = QString("長いパスでも文字を保持").toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
            tatsu::fail("Cannot write long private path");
        file.close();
        if (!file.open(QIODevice::ReadOnly) || file.readAll() != content)
            tatsu::fail("Long private path changed content");
    }
    if (QFileInfo::exists(temporary))
        tatsu::fail("Long private directory not reclaimed");
    tatsu::Document document;
    document.open(fixtures + "/D01.pdf");
    const auto original = tatsu::fileHash(document.source);
    document.putSignature(0, "長い保存先への署名", {60, 100}, 18, Qt::black);
    const auto before = tatsu::renderPage(document.pdf(), 0, 1);
    const auto path = directory + "/保存したPDF.pdf";
    document.save(path);
    document.rotate(0);
    document.save(path);
    tatsu::Document reopened;
    reopened.open(path);
    if (reopened.pages() != 1 || tatsu::signatures(reopened.pdf(), 0).size() != 1 ||
        tatsu::fileHash(document.source) != original)
        tatsu::fail("Long save did not preserve signature or source");
    document.undo();
    document.save(path);
    reopened.open(path);
    if (tatsu::renderPage(reopened.pdf(), 0, 1) != before)
        tatsu::fail("Long-path overwrite and Undo changed appearance");
    return {{"AppContainer", tatsu::runningInAppContainer()},
            {"path_characters", path.size()},
            {"private_read_write_cleanup", true},
            {"save_replace_reopen_undo", true},
            {"original_preserved", true}};
}
