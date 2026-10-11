#include "text_font.h"
#include "document.h"
#include <QtEndian>

namespace tatsu
{
namespace
{
void loadSystemTextFonts()
{
#ifdef Q_OS_WIN
    // Qt's offscreen backend does not enumerate the Windows font service.
    // Register the installed, curated faces in process only; never copy their
    // programs to assets or require an optional Windows font download.
    static const bool loaded = []
    {
        const auto directory = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows") + "/Fonts");
        for (const auto& name : {"meiryo.ttc", "YuGothR.ttc", "yumin.ttf", "msgothic.ttc",
                                 "msmincho.ttc", "arial.ttf", "times.ttf"})
        {
            const auto path = directory.filePath(name);
            if (QFileInfo::exists(path))
                QFontDatabase::addApplicationFont(path);
        }
        return true;
    }();
    Q_UNUSED(loaded);
#endif
}
} // namespace
bool allowsEditableFontEmbedding(const QByteArray& os2Table)
{
    if (os2Table.size() < 10)
        return false;
    const auto flags = qFromBigEndian<quint16>(os2Table.constData() + 8);
    // This application writes subsets and keeps its added text editable.
    // Preview/print-only, bitmap-only and no-subsetting fonts cannot satisfy that.
    const auto permissions = flags & 0x000e;
    return (permissions == 0 || (permissions & 0x0008)) && !(flags & 0x0300);
}
QFont textFont(const QString& family)
{
    loadSystemTextFonts();
    const QString requested = family.isEmpty() ? signatureFont() : family;
    const auto available = QFontDatabase::families();
    if (!available.contains(requested, Qt::CaseInsensitive))
        fail("書体「" + requested +
             "」はこのPCにありません。別の書体を選んでください。"
             "保存済みPDFの表示は保持されます。");
    QFont font(requested);
    font.setPixelSize(100);
    font.setStyleStrategy(QFont::NoFontMerging);
    const auto raw = QRawFont::fromFont(font);
    if (!raw.isValid() || raw.familyName().compare(requested, Qt::CaseInsensitive) != 0)
        fail("指定した書体を読み込めません。別の書体を選んでください。");
    if (!allowsEditableFontEmbedding(raw.fontTable("OS/2")))
        fail("書体「" + requested +
             "」は再編集可能なPDFへの埋め込みに対応していません。"
             "Noto Sans JPなど別の書体を選んでください。");
    return font;
}
QStringList textFontFamilies()
{
    const QStringList candidates = {"Meiryo UI", "Meiryo",          "Yu Gothic",
                                    "Yu Mincho", "MS Gothic",       "MS Mincho",
                                    "Arial",     "Times New Roman", signatureFont()};
    QStringList result;
    for (const auto& family : candidates)
    {
        try
        {
            textFont(family);
            result.append(family);
        }
        catch (const std::exception&)
        {
            // Only offer installed faces that this writer can embed safely.
        }
    }
    return result;
}
} // namespace tatsu
