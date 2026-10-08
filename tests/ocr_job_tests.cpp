#include "ocr_job_tests.h"
#include "document.h"
#include "ocr_jobs.h"
#include "private_temp.h"

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
