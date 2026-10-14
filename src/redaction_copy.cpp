#include "redaction_copy.h"
#include "save_candidate.h"
#include "windows_path.h"
#include <windows.h>

namespace tatsu
{
namespace
{
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("墨消しを中止しました。元の文書は変更していません。");
}
} // namespace
RedactedCopy exportRedactedPdf(const PDFDocument& document,
                               const QMap<int, QVector<QRectF>>& regions,
                               const QString& destination, const std::function<bool()>& cancelled,
                               const std::function<void(QString)>& progress,
                               const std::function<void()>& validateInputs)
{
    stop(cancelled);
    if (validateInputs)
        validateInputs();
    if (destination.isEmpty() || !destination.endsWith(".pdf", Qt::CaseInsensitive) ||
        QFileInfo::exists(destination) || !QFileInfo(destination).dir().exists())
        fail("存在するフォルダに、まだ使われていない.pdfの保存先を選んでください。");
    int count = 0;
    for (const auto& page : regions)
    {
        if (page.size() > 1000 - count)
            fail("墨消しする範囲はコピー全体で1〜1000個にしてください。");
        count += int(page.size());
    }
    if (count < 1 || count > 1000)
        fail("墨消しする範囲はコピー全体で1〜1000個にしてください。");
    if (progress)
        progress("指定範囲の内容と隠れた情報を削除し、残る資源を確認しています…");
    const auto candidate = prepareRedactionCandidate(document, regions, cancelled);
    stop(cancelled);
    const auto bytes = encodePdf(candidate.document);
    if (bytes.size() > 256 * 1024 * 1024)
        fail("墨消ししたコピーが256MiBの保存上限を超えています。");
    SaveCandidate staged(QFileInfo(destination).absolutePath());
    QFile file(staged.filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(bytes) != bytes.size() || !file.flush())
        fail("墨消しした保存候補を書き込めません。元の文書は保持しています。");
    file.close();
    stop(cancelled);
    if (progress)
        progress("保存候補を開き直し、ページと座標を確認しています…");
    const auto reopened = readPdf(staged.filePath());
    if (reopened.getCatalog()->getPageCount() != document.getCatalog()->getPageCount())
        fail("墨消ししたコピーのページ数を確認できません。");
    for (int index = 0; index < int(document.getCatalog()->getPageCount()); ++index)
    {
        stop(cancelled);
        const auto before = document.getCatalog()->getPage(index);
        const auto after = reopened.getCatalog()->getPage(index);
        if (before->getMediaBox() != after->getMediaBox() ||
            before->getCropBox() != after->getCropBox() ||
            before->getPageRotation() != after->getPageRotation() ||
            before->getUserUnit() != after->getUserUnit())
            fail("墨消ししたコピーのページ座標を確認できません。");
    }
    const auto hash = fileHash(staged.filePath());
    if (hash != QCryptographicHash::hash(bytes, QCryptographicHash::Sha256))
        fail("墨消ししたコピーの保存バイト列を確認できません。");
    if (progress)
        progress("墨消ししたコピーを新しいPDFへ保存しています…");
    stop(cancelled);
    if (validateInputs)
        validateInputs();
    const auto from = extendedWindowsPath(staged.filePath());
    const auto to = extendedWindowsPath(QFileInfo(destination).absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString(
                 "墨消ししたコピーを確定できません（Windows %1）。既存ファイルは変更していません。")
                 .arg(GetLastError()));
    return {hash,
            candidate.textSegments,
            candidate.images,
            candidate.fieldGroups,
            candidate.annotations,
            candidate.contentGroups};
}
} // namespace tatsu
