#include "ocr_job_tests.h"
#include "document.h"
#include "ocr_jobs.h"

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
