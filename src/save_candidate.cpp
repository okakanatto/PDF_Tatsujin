#include "save_candidate.h"
#include "document.h"
#include <QDirIterator>

namespace tatsu
{
namespace
{
const QByteArray owner("PDFTatsujin save candidate v1\n");
bool ownedCandidate(const QString& path)
{
    // Reconstruct QFileInfo on each check; do not reuse the iterator's cached
    // metadata after acquiring the lease.
    const QFileInfo directory(path);
    static const QRegularExpression name("^\\.pdf-tatsujin-save-[A-Za-z0-9]{6,32}$");
    if (!directory.isDir() || directory.isSymLink() || directory.isJunction() ||
        !name.match(directory.fileName()).hasMatch())
        return false;
    const QDir root(directory.absoluteFilePath());
    const auto entries =
        root.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto& entry : entries)
        if (!entry.isFile() || entry.isSymLink() || entry.isJunction() ||
            !QStringList{".tatsujin-owner", "candidate.pdf", "save.lock"}.contains(
                entry.fileName()))
            return false;
    QFile marker(root.filePath(".tatsujin-owner"));
    return marker.open(QIODevice::ReadOnly) && marker.read(owner.size() + 1) == owner;
}
} // namespace
QStringList cleanAbandonedSaveCandidates(const QString& parent)
{
    QStringList removed;
    const QDir root(parent);
    QDirIterator entries(root.absolutePath(), {".pdf-tatsujin-save-*"},
                         QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    // A folder full of unrelated names must not turn saving into an unbounded scan.
    for (int examined = 0; entries.hasNext() && examined < 128; ++examined)
    {
        entries.next();
        const auto info = entries.fileInfo();
        if (!ownedCandidate(info.absoluteFilePath()))
            continue;
        const QDir candidate(info.absoluteFilePath());
        QLockFile lease(candidate.filePath("save.lock"));
        lease.setStaleLockTime(0); // Never expire a live writer based on elapsed time.
        if (!lease.tryLock() || !ownedCandidate(info.absoluteFilePath()))
            continue;
        // Delete only the two approved files. A new or unknown entry is never
        // recursively removed, and prevents removal of the directory itself.
        const auto pdf = candidate.filePath("candidate.pdf");
        if (QFileInfo::exists(pdf) && !QFile::remove(pdf))
            continue;
        if (!QFile::remove(candidate.filePath(".tatsujin-owner")))
            continue;
        lease.unlock();
        if (root.rmdir(info.fileName()))
            removed.append(info.fileName());
    }
    return removed;
}
SaveCandidate::SaveCandidate(const QString& parent)
{
    cleanAbandonedSaveCandidates(parent);
    directory = privateTemporaryDirectory(QDir(parent).filePath(".pdf-tatsujin-save-XXXXXX"));
    if (!directory->isValid())
        fail("保存先に一時領域を作成できません。");
    lease = std::make_unique<QLockFile>(directory->filePath("save.lock"));
    lease->setStaleLockTime(0);
    if (!lease->tryLock())
        fail("保存の一時領域を使用できません。");
    // Publish ownership only after the writer holds its process lease.
    QFile marker(directory->filePath(".tatsujin-owner"));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        marker.write(owner) != owner.size() || !marker.flush())
        fail("保存の一時領域を識別できません。");
}
QString SaveCandidate::filePath() const
{
    return directory->filePath("candidate.pdf");
}
} // namespace tatsu
