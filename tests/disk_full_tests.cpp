#include "disk_full_tests.h"
#include "document.h"
#include "windows_path.h"
#include <QScopeGuard>
#include <windows.h>

namespace tatsu
{
namespace
{
void check(bool value, const QString& reason)
{
    if (!value)
        fail(reason);
}
quint64 freeBytes(const QString& path)
{
    ULARGE_INTEGER available{}, total{}, free{};
    const auto native = extendedWindowsPath(path);
    check(GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(native.utf16()), &available, &total, &free),
          "Cannot query the owned test volume");
    check(total.QuadPart >= 8 * 1024 * 1024 && total.QuadPart <= 64 * 1024 * 1024,
          "Capacity test refuses volumes outside the 8–64 MiB range");
    return available.QuadPart;
}
} // namespace
QJsonObject testDiskFullSave(const QString& fixtures, const QString& volumeRoot)
{
    const auto rootPath = QFileInfo(volumeRoot).absoluteFilePath();
    const auto native = extendedWindowsPath(rootPath + "/");
    wchar_t label[64]{};
    wchar_t filesystem[64]{};
    check(GetVolumeInformationW(reinterpret_cast<LPCWSTR>(native.utf16()), label, 64, nullptr,
                                nullptr, nullptr, filesystem, 64) &&
              QString::fromWCharArray(label) == "TATSU-TEST" &&
              QString::fromWCharArray(filesystem) == "NTFS",
          "Capacity test requires the newly created TATSU-TEST volume");
    QFile marker(rootPath + "/.tatsu-capacity-owner");
    check(marker.open(QIODevice::ReadOnly), "Capacity volume has no ownership marker");
    const auto ownership = marker.readAll();
    const QByteArray ownerPrefix("PDFTatsujin capacity probe v1\n");
    check(ownership.startsWith(ownerPrefix) &&
              !QUuid(QString::fromUtf8(ownership.mid(ownerPrefix.size())).trimmed()).isNull(),
          "Invalid capacity volume ownership marker");
    const auto initiallyFree = freeBytes(rootPath);
    QTemporaryDir work(rootPath + "/probe-XXXXXX");
    check(work.isValid(), "Cannot create an owned capacity probe directory");
    const auto source = fixtures + "/D03.pdf", small = fixtures + "/D01.pdf";
    const auto sourceHash = fileHash(source), smallHash = fileHash(small);
    const auto existing = work.filePath("existing.pdf"), fresh = work.filePath("new.pdf");
    check(QFile::copy(small, existing), "Cannot prepare an existing synthetic destination");
    const auto beforeFile = fileHash(existing);
    Document document;
    document.open(source);
    document.putSignature(0, "容量不足の署名", {60, 100}, 14, Qt::black);
    const auto beforePdf = encodePdf(document.pdf());
    const auto beforeImage = renderPage(document.pdf(), 0, 1);
    const auto revision = document.revision;
    const auto history = document.history.size();
    const auto cursor = document.cursor, saved = document.saved;
    const auto beforeTarget = document.target;
    const auto beforeTargetHash = document.targetHash;
    const auto fillerPath = work.filePath("owned-filler.bin");
    QFile filler(fillerPath);
    check(filler.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Cannot create owned filler");
    const auto cleanupFiller = qScopeGuard(
        [&]
        {
            filler.close();
            QFile::remove(fillerPath);
        });
    const QByteArray block(1024 * 1024, 'x');
    constexpr quint64 remaining = 64 * 1024;
    while (true)
    {
        const auto available = freeBytes(rootPath);
        if (available <= remaining)
            break;
        const auto amount = qint64(qMin(quint64(block.size()), available - remaining));
        check(filler.write(block.constData(), amount) == amount && filler.flush(),
              "Filler did not reach its controlled free-space target");
    }
    const auto beforeAttemptFree = freeBytes(rootPath);
    check(beforeAttemptFree < quint64(beforePdf.size()),
          "PDF fits in the test volume unexpectedly");
    QJsonArray failures;
    for (const auto& target : {existing, fresh})
    {
        QString error;
        try
        {
            document.save(target, target == existing ? beforeFile : QByteArray());
        }
        catch (const std::exception& failure)
        {
            error = QString::fromUtf8(failure.what());
        }
        check(!error.isEmpty(), "Expected failure on the genuinely space-limited NTFS volume");
        check(fileHash(existing) == beforeFile && !QFileInfo::exists(fresh) &&
                  encodePdf(document.pdf()) == beforePdf && document.dirty() &&
                  document.revision == revision && document.history.size() == history &&
                  document.cursor == cursor && document.saved == saved &&
                  document.target == beforeTarget && document.targetHash == beforeTargetHash,
              "Disk-full failure changed the PDF, existing destination, history or save state");
        check(QDir(work.path())
                  .entryList({".pdf-tatsujin-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                  .isEmpty(),
              "Disk-full failure left a candidate directory");
        failures.append(QJsonObject{{"destination", target == existing ? "existing" : "new"},
                                    {"error", error},
                                    {"state_and_file_preserved", true}});
    }
    filler.close();
    check(QFile::remove(fillerPath), "Cannot release the owned filler file");
    document.undo();
    document.redo();
    check(encodePdf(document.pdf()) == beforePdf && document.dirty(), "Failure damaged Undo/Redo");
    document.save(existing, beforeFile);
    Document reopened;
    reopened.open(existing);
    check(!document.dirty() && signatures(reopened.pdf(), 0).size() == 1 &&
              renderPage(reopened.pdf(), 0, 1) == beforeImage && fileHash(source) == sourceHash &&
              fileHash(small) == smallHash,
          "Retry after freeing space changed appearance, editability or fixed inputs");
    return {{"status", "PASS"},
            {"initial_free_bytes", qint64(initiallyFree)},
            {"free_before_attempt_bytes", qint64(beforeAttemptFree)},
            {"requested_PDF_bytes", beforePdf.size()},
            {"failures", failures},
            {"release_retry_reopen_undo", true},
            {"source_inputs_unchanged", true},
            {"scope",
             "Actual space limitation in newly created 64 MiB NTFS VHD; API probe, no GUI input"}};
}
} // namespace tatsu
