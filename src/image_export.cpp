#include "image_export.h"
#include "pdfexception.h"
#include "pdfsecurityhandler.h"
#include "private_temp.h"
#include "windows_path.h"
#include <cmath>
#include <windows.h>

namespace tatsu
{
QSize imageExportSize(const PDFDocument& document, int page, int dpi)
{
    if (!document.getCatalog() || page < 0 || page >= int(document.getCatalog()->getPageCount()) ||
        dpi < 75 || dpi > 600)
        fail("ページと75〜600dpiの解像度を指定してください。");
    const auto dimensions = pageSize(document.getCatalog()->getPage(page)) * (dpi / 72.0);
    if (!std::isfinite(dimensions.width()) || !std::isfinite(dimensions.height()) ||
        dimensions.width() < .5 || dimensions.height() < .5 || dimensions.width() > 64'000'000 ||
        dimensions.height() > 64'000'000)
        fail("ページの出力寸法が不正か、大きすぎます。");
    const auto pixels = dimensions.toSize();
    if (qint64(pixels.width()) * pixels.height() > 64'000'000)
        fail("1画像64メガピクセルを超えます。解像度を下げてください。");
    return pixels;
}
ImageExportOutcome exportImages(PDFDocument document, const QVector<int>& pages,
                                const ImageExportOptions& options,
                                const std::function<bool()>& cancelled,
                                const std::function<void(const QString&, int, int)>& progress)
try
{
    if (!document.getCatalog() || !document.getStorage().getSecurityHandler()->isAllowed(
                                      pdf::PDFSecurityHandler::Permission::CopyContent))
        fail("この文書では画像の出力が許可されていません。");
    if (pages.isEmpty() || pages.size() > 128)
        fail("1〜128ページの出力範囲を指定してください。");
    if (options.format != "png" && options.format != "jpeg")
        fail("PNGまたはJPEGを指定してください。");
    if (options.prefix.trimmed().isEmpty() || options.prefix.size() > 80 ||
        options.prefix.contains(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]")) ||
        options.prefix.endsWith('.') || options.prefix.endsWith(' '))
        fail("出力名は80文字までのファイル名を指定してください。区切り文字は使えません。");
    const QDir directory(QFileInfo(options.directory).absoluteFilePath());
    if (options.directory.isEmpty() || !directory.exists())
        fail("存在する出力フォルダを選んでください。");
    ImageExportOutcome outcome;
    QSet<int> selected;
    QVector<QSize> sizes;
    for (int page : pages)
    {
        if (selected.contains(page))
            fail("出力範囲に重複ページがあります。");
        selected.insert(page);
        sizes.append(imageExportSize(document, page, options.dpi));
        const auto path =
            directory.absoluteFilePath(QString("%1-%2.%3")
                                           .arg(options.prefix)
                                           .arg(page + 1, 4, 10, QChar('0'))
                                           .arg(options.format == "png" ? "png" : "jpg"));
        if (QFileInfo::exists(path))
            fail("同じ名前の出力が既にあります。名前かフォルダを変更してください: " +
                 QFileInfo(path).fileName());
        outcome.files.append({page, path, "未出力", false});
    }
    auto temporary =
        privateTemporaryDirectory(directory.absoluteFilePath("pdf-tatsujin-image-export-XXXXXX"));
    if (!temporary->isValid())
        fail("出力フォルダに私有作業領域を作れません。");
    QStringList staged;
    for (int i = 0; i < pages.size(); ++i)
    {
        if (cancelled && cancelled())
        {
            outcome.cancelled = true;
            return outcome;
        }
        if (progress)
            progress("描画", i, int(pages.size()));
        if (cancelled && cancelled())
        {
            outcome.cancelled = true;
            return outcome;
        }
        auto image = renderPage(document, pages[i], options.dpi / 72.0);
        if (image.size() != sizes[i])
            fail("描画した画像の寸法が予定と異なります。");
        const int dotsPerMeter = qRound(options.dpi * 1000.0 / 25.4);
        image.setDotsPerMeterX(dotsPerMeter);
        image.setDotsPerMeterY(dotsPerMeter);
        const auto path = temporary->filePath(QString("page-%1").arg(i));
        QImageWriter writer(path, options.format);
        if (options.format == "jpeg")
            writer.setQuality(95);
        if (!writer.write(image))
            fail("画像を保存できません: " + writer.errorString());
        QImageReader check(path, options.format);
        if (!check.canRead() || check.size() != sizes[i] || check.read().isNull())
            fail("保存候補の画像を検証できません。");
        staged.append(path);
    }
    for (int i = 0; i < staged.size(); ++i)
    {
        if (cancelled && cancelled())
        {
            outcome.cancelled = true;
            return outcome;
        }
        if (progress)
            progress("保存", i, int(staged.size()));
        if (cancelled && cancelled())
        {
            outcome.cancelled = true;
            return outcome;
        }
        // No REPLACE_EXISTING flag: a concurrent writer keeps its file.
        const auto from = extendedWindowsPath(staged[i]);
        const auto to = extendedWindowsPath(outcome.files[i].path);
        if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                         reinterpret_cast<LPCWSTR>(to.utf16()), MOVEFILE_WRITE_THROUGH))
        {
            outcome.files[i].error =
                QString("画像の確定に失敗しました（Windows %1）。既存ファイルは変更していません。")
                    .arg(GetLastError());
            return outcome;
        }
        outcome.files[i].success = true;
        outcome.files[i].error.clear();
    }
    return outcome;
}
catch (const pdf::PDFException& error)
{
    fail("PDFの画像出力に失敗しました: " + error.getMessage());
}
} // namespace tatsu
