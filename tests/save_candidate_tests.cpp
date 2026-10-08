#include "save_candidate_tests.h"
#include "document.h"
#include "save_candidate.h"
#include "windows_path.h"
#include <QScopeGuard>
#include <windows.h>

namespace tatsu
{
QJsonObject testSaveCandidateCleanup(const QString& fixtures, const QString& output)
{
    auto check = [](bool value, const QString& message)
    {
        if (!value)
            fail(message);
    };
    QTemporaryDir root(output + "/save-cleanup-XXXXXX");
    QTemporaryDir external(output + "/save-unrelated-XXXXXX");
    check(root.isValid() && external.isValid(), "create isolated save cleanup roots");
    Document document;
    document.open(fixtures + "/D01.pdf");
    const auto original = fileHash(document.source);
    auto make = [&](const QString& suffix, const QByteArray& marker)
    {
        const auto path = root.filePath(".pdf-tatsujin-save-" + suffix);
        check(QDir().mkpath(path), "create synthetic cleanup candidate");
        QFile file(path + "/.tatsujin-owner");
        check(file.open(QIODevice::WriteOnly) && file.write(marker) == marker.size(),
              "write test ownership marker");
        file.close();
        writeCandidate(document.pdf(), path + "/candidate.pdf");
        return path;
    };
    const auto abandoned = make("abandoned", "PDFTatsujin save candidate v1\n");
    const auto unrelated = make("unrelated", "Another application's marker\n");
    const auto unknown = make("unknown", "PDFTatsujin save candidate v1\n");
    check(QFile::copy(fixtures + "/D01.pdf", unknown + "/user-file.pdf"),
          "prepare unrelated file inside a matching folder");
    const auto nested = make("nested", "PDFTatsujin save candidate v1\n");
    check(QDir().mkpath(nested + "/subdirectory"), "prepare unsupported nested contents");
    const auto partial = make("partial", "PDFTatsujin save candidate");
    const auto extra = make("appended", "PDFTatsujin save candidate v1\nx");
    const auto unmarked = make("unmarked", "PDFTatsujin save candidate v1\n");
    check(QFile::remove(unmarked + "/.tatsujin-owner"), "prepare markerless candidate");
    const auto legacy = root.filePath(".pdf-tatsujin-legacy");
    check(QDir().mkpath(legacy) && QFile::copy(fixtures + "/D01.pdf", legacy + "/candidate.pdf"),
          "prepare markerless candidate from a previous version");
    const auto outside = external.filePath("original.pdf");
    check(QFile::copy(fixtures + "/D01.pdf", outside), "prepare junction target original");
    QFile marker(external.filePath(".tatsujin-owner"));
    check(marker.open(QIODevice::WriteOnly) &&
              marker.write("PDFTatsujin save candidate v1\n") == 30,
          "write synthetic junction target marker");
    marker.close();
    const auto junction = root.filePath(".pdf-tatsujin-save-junction");
    check(QProcess::execute("cmd.exe",
                            {"/d", "/c", "mklink", "/J", QDir::toNativeSeparators(junction),
                             QDir::toNativeSeparators(external.path())}) == 0 &&
              QFileInfo(junction).isJunction(),
          "create a real owned Windows junction");
    const auto nativeJunction = extendedWindowsPath(junction);
    const auto removeJunction =
        qScopeGuard([&] { RemoveDirectoryW(reinterpret_cast<LPCWSTR>(nativeJunction.utf16())); });
    const auto outsideHash = fileHash(outside);
    const auto retained = QStringList{unrelated, unknown, nested, partial, extra, unmarked, legacy};
    QMap<QString, QByteArray> hashes;
    for (const auto& path : retained)
        hashes[path] = fileHash(path + "/candidate.pdf");
    QString active;
    {
        SaveCandidate live(root.path());
        active = QFileInfo(live.filePath()).absolutePath();
        writeCandidate(document.pdf(), live.filePath());
        check(!QFileInfo::exists(abandoned), "retry creation removes an abandoned owned candidate");
        const auto liveHash = fileHash(live.filePath());
        check(cleanAbandonedSaveCandidates(root.path()).isEmpty() &&
                  fileHash(live.filePath()) == liveHash,
              "cleanup preserves a live writer without time-based expiry");
        SaveCandidate second(root.path());
        check(QFileInfo::exists(live.filePath()), "another save retains the first active writer");
        for (const auto& path : retained)
            check(fileHash(path + "/candidate.pdf") == hashes[path],
                  "cleanup preserves unknown, malformed and unrelated data");
        check(QFileInfo(junction).isJunction() && fileHash(outside) == outsideHash,
              "cleanup never follows a real directory junction");
    }
    check(!QFileInfo::exists(active), "normal lifetime removes the owned save directory");
    QString failedPath;
    try
    {
        SaveCandidate failed(root.path());
        failedPath = QFileInfo(failed.filePath()).absolutePath();
        writeCandidate(document.pdf(), failed.filePath());
        throw std::runtime_error("controlled test failure after candidate write");
    }
    catch (const std::runtime_error&)
    {
    }
    check(!failedPath.isEmpty() && !QFileInfo::exists(failedPath),
          "exception unwinding removes its own candidate");
    document.putSignature(0, "保存後も再編集", {60, 100}, 18, Qt::black);
    document.save(root.filePath("saved.pdf"));
    Document reopened;
    reopened.open(root.filePath("saved.pdf"));
    check(signatures(reopened.pdf(), 0).front().text == "保存後も再編集" &&
              fileHash(document.source) == original,
          "normal save preserves source and editable Japanese signature");
    return {{"retry_cleanup", true},
            {"two_live_writers_preserved", true},
            {"unknown_and_malformed_preserved", retained.size()},
            {"real_windows_junction_preserved", true},
            {"normal_save_cleanup_and_reedit", true}};
}
} // namespace tatsu
