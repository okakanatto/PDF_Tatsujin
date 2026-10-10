#include "office_package.h"
#include "document.h"
#include "private_temp.h"
#include "windows_path.h"
#include <QtCore/private/qzipreader_p.h>
#include <QtCore/private/qzipwriter_p.h>
#include <windows.h>

namespace tatsu
{
void writeOfficePackage(const QMap<QString, QByteArray>& parts, const QString& path,
                        const QString& extension, const std::function<bool()>& cancelled)
{
    const auto stop = [&]
    {
        if (cancelled && cancelled())
            fail("文書の出力を取り消しました。");
    };
    stop();
    if (parts.isEmpty() || parts.size() > 256)
        fail("文書の構成が不正です。");
    qsizetype bytes = 0;
    for (auto part = parts.cbegin(); part != parts.cend(); ++part)
    {
        const auto segments = part.key().split('/');
        if (part.key().contains('\\') || segments.contains("..") || segments.contains(".") ||
            segments.contains(QString()) || (bytes += part.value().size()) > 64 * 1024 * 1024)
            fail("文書の構成またはサイズが不正です。");
    }
    const QFileInfo destination(path);
    if (destination.suffix().compare(extension, Qt::CaseInsensitive) != 0 || destination.exists() ||
        destination.isSymLink())
        fail("新しい." + extension + "ファイルを指定してください。既存ファイルは上書きしません。");
    auto temporary =
        privateTemporaryDirectory(destination.absolutePath() + "/PDFTatsujin-office-XXXXXX");
    if (!temporary->isValid())
        fail("文書出力の作業フォルダを作成できません。");
    const auto candidate = temporary->filePath("candidate." + extension);
    {
        QZipWriter zip(candidate);
        for (auto part = parts.cbegin(); part != parts.cend(); ++part)
        {
            stop();
            zip.addFile(part.key(), part.value());
        }
        zip.close();
        if (zip.status() != QZipWriter::NoError)
            fail("文書ファイルを作成できません。");
    }
    {
        QZipReader zip(candidate);
        if (zip.fileInfoList().size() != parts.size())
            fail("完成候補の構成が一致しません。");
        for (auto part = parts.cbegin(); part != parts.cend(); ++part)
        {
            stop();
            if (zip.fileData(part.key()) != part.value())
                fail("完成候補の内容が一致しません。");
        }
        if (zip.status() != QZipReader::NoError)
            fail("完成候補を検査できません。");
    }
    stop();
    const auto from = extendedWindowsPath(candidate),
               to = extendedWindowsPath(destination.absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString("文書を保存できません。既存ファイルは保持しました。（Windows %1）")
                 .arg(GetLastError()));
}
} // namespace tatsu
