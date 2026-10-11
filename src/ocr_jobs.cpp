#include "ocr_jobs.h"
#include "document.h"
#include <QDirIterator>

namespace tatsu
{
namespace
{
bool ownedJob(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink() || info.isJunction() ||
        !info.fileName().startsWith("pdf-tatsujin-job-"))
        return false;
    const QFileInfo markerInfo(path + "/.tatsujin-owner");
    if (!markerInfo.isFile() || markerInfo.isSymLink())
        return false;
    QFile marker(markerInfo.absoluteFilePath());
    return marker.open(QIODevice::ReadOnly) && marker.read(64) == "PDFTatsujin job v1";
}
} // namespace
QStringList cleanAbandonedOcrJobs(const QString& root)
{
    QStringList removed;
    const QDir directory(root);
    for (const auto& info : directory.entryInfoList(
             {"pdf-tatsujin-job-*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
    {
        const auto path = info.absoluteFilePath();
        if (!ownedJob(path))
            continue;
        bool links = false;
        QDirIterator entries(path,
                             QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                             QDirIterator::Subdirectories);
        while (entries.hasNext() && !links)
        {
            entries.next();
            links = entries.fileInfo().isSymLink() || entries.fileInfo().isJunction();
        }
        if (links)
            continue;
        QLockFile parent(path + "/job.lock"), worker(path + "/worker.lock");
        parent.setStaleLockTime(0);
        worker.setStaleLockTime(0);
        if (!parent.tryLock() || !worker.tryLock())
            continue;
        worker.unlock();
        parent.unlock();
        if (QDir(path).removeRecursively())
            removed.append(info.fileName());
    }
    return removed;
}
std::unique_ptr<QLockFile> lockOwnedOcrWorker(const QString& input)
{
    const QFileInfo file(input);
    const auto path = file.absolutePath();
    if (file.fileName() != "input.pdf" || !ownedJob(path))
        return {};
    auto lock = std::make_unique<QLockFile>(path + "/worker.lock");
    lock->setStaleLockTime(0);
    if (!lock->tryLock())
        fail("OCR作業領域を別のワーカーが使用しています。");
    return lock;
}
} // namespace tatsu
