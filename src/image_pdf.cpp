#include "image_pdf.h"
#include "image_embedding.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include <QtEndian>
#include <cmath>

namespace tatsu
{
namespace
{
constexpr qint64 maxBytes = 64LL * 1024 * 1024;
constexpr qint64 maxPixels = 32'000'000;
QByteArray inputBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("画像を読み取れません: " + QFileInfo(path).fileName() + "\n" + file.errorString());
    if (file.size() > maxBytes)
        fail("1画像は64MiBまでです: " + QFileInfo(path).fileName());
    auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes || !file.atEnd() || file.error() != QFile::NoError)
        fail("画像を最後まで読み取れません: " + QFileInfo(path).fileName());
    // A static PNG decoder may ignore APNG controls and return only its first
    // picture. Recognize the declared format before decoding any image.
    if (bytes.startsWith(QByteArrayLiteral("\x89PNG\r\n\x1a\n")))
        for (qsizetype offset = 8; offset <= bytes.size() - 12;)
        {
            const auto length = qFromBigEndian<quint32>(bytes.constData() + offset);
            if (length > quint64(bytes.size() - offset - 12))
                break;
            const auto type = bytes.mid(offset + 4, 4);
            if (type == "acTL")
                fail("アニメーションPNGには対応していません。静止画像を選んでください: " +
                     QFileInfo(path).fileName());
            if (type == "IEND")
                break;
            offset += qsizetype(length) + 12;
        }
    return bytes;
}
QByteArray hash(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
qint64 checkReader(QImageReader& reader, const QString& path)
{
    const auto format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg")
        fail("PNG・JPEGの静止画像を選んでください: " + QFileInfo(path).fileName());
    const auto size = reader.size();
    const qint64 pixels = qint64(size.width()) * size.height();
    if (size.width() <= 0 || size.height() <= 0 || pixels > maxPixels)
        fail("画像の寸法が不正か、1画像32メガピクセルを超えています: " +
             QFileInfo(path).fileName());
    if (reader.supportsAnimation() || reader.imageCount() > 1)
        fail("複数フレームの画像には対応していません: " + QFileInfo(path).fileName());
    reader.setAutoTransform(true);
    return pixels;
}
void checkCancel(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("作成を中止しました。入力画像は変更していません。");
}
} // namespace
ImagePdfLayout imagePdfLayout(QSize pixels, const ImagePdfOptions& options)
{
    if (!std::isfinite(options.dpi) || options.dpi < 75 || options.dpi > 600 ||
        pixels.width() <= 0 || pixels.height() <= 0)
        fail("画像の寸法と75〜600dpiの解像度を指定してください。");
    if (options.pageMode == ImagePdfOptions::PageMode::ImageSize)
    {
        const QSizeF page = QSizeF(pixels) * (72.0 / options.dpi);
        if (qMax(page.width(), page.height()) > 14400)
            fail("画像サイズの用紙は一辺14,400ptまでです。解像度を上げるかA4を選んでください。");
        return {page, QRectF(QPointF(), page)};
    }
    if (options.pageMode != ImagePdfOptions::PageMode::AutoA4)
        fail("用紙の設定が不正です。");
    constexpr double ptPerMm = 72.0 / 25.4;
    const QSizeF page = pixels.width() > pixels.height() ? QSizeF(297 * ptPerMm, 210 * ptPerMm)
                                                         : QSizeF(210 * ptPerMm, 297 * ptPerMm);
    const double margin = 10 * ptPerMm;
    const QSizeF room = page - QSizeF(2 * margin, 2 * margin);
    const double scale = qMin(room.width() / pixels.width(), room.height() / pixels.height());
    const QSizeF fitted = QSizeF(pixels) * scale;
    return {page, QRectF(QPointF((page.width() - fitted.width()) / 2,
                                 (page.height() - fitted.height()) / 2),
                         fitted)};
}
QImage imagePdfPreview(const QString& path, QByteArray* expected)
{
    auto bytes = inputBytes(path);
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    checkReader(reader, path);
    reader.setScaledSize(reader.size().scaled(240, 160, Qt::KeepAspectRatio));
    auto image = reader.read();
    if (image.isNull())
        fail("画像を表示できません: " + reader.errorString());
    if (expected)
        *expected = hash(bytes);
    return image;
}
PDFDocument createImagePdf(const QVector<ImagePdfInput>& inputs, const ImagePdfOptions& options,
                           const std::function<bool()>& cancelled,
                           const std::function<void(int, int, const QString&)>& progress)
try
{
    if (inputs.isEmpty() || inputs.size() > 128)
        fail("1〜128枚の画像を指定してください。");
    imagePdfLayout({1, 1}, options);
    QVector<QByteArray> baselines;
    qint64 totalPixels = 0;
    for (const auto& input : inputs)
    {
        checkCancel(cancelled);
        auto bytes = inputBytes(input.path);
        const auto digest = hash(bytes);
        if (!input.expectedHash.isEmpty() && digest != input.expectedHash)
            fail("プレビュー後に画像が変更されました。選び直してください: " +
                 QFileInfo(input.path).fileName());
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        totalPixels += checkReader(reader, input.path);
        if (totalPixels > 128'000'000)
            fail("画像の合計は128メガピクセルまでです。分けて作成してください。");
        baselines.append(digest);
    }
    pdf::PDFDocumentBuilder builder;
    for (int i = 0; i < inputs.size(); ++i)
    {
        checkCancel(cancelled);
        auto bytes = inputBytes(inputs[i].path);
        if (hash(bytes) != baselines[i])
            fail("作成中に画像が変更されました: " + QFileInfo(inputs[i].path).fileName());
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const auto pixels = checkReader(reader, inputs[i].path);
        auto image = reader.read();
        if (image.isNull() || qint64(image.width()) * image.height() != pixels)
            fail("画像を読み込めません: " + QFileInfo(inputs[i].path).fileName() + "\n" +
                 reader.errorString());
        checkCancel(cancelled);
        const auto layout = imagePdfLayout(image.size(), options);
        const auto page = builder.appendPage(QRectF(QPointF(), layout.page));
        const auto imageRef = embedImage(builder, image);
        using namespace detail;
        pdf::PDFDictionary objects, resources;
        set(objects, "Image", pdf::PDFObject::createReference(imageRef));
        set(resources, "XObject", dictObject(objects));
        const auto commands = QString("q %1 0 0 %2 %3 %4 cm /Image Do Q\n")
                                  .arg(layout.image.width(), 0, 'g', 14)
                                  .arg(layout.image.height(), 0, 'g', 14)
                                  .arg(layout.image.x(), 0, 'g', 14)
                                  .arg(layout.page.height() - layout.image.bottom(), 0, 'g', 14)
                                  .toLatin1();
        builder.setObject(
            page,
            [&]
            {
                auto dictionary = *builder.getObjectByReference(page).getDictionary();
                set(dictionary, "Resources", dictObject(resources));
                set(dictionary, "Contents",
                    pdf::PDFObject::createReference(builder.addObject(streamObject({}, commands))));
                return dictObject(dictionary);
            }());
        if (progress)
            progress(i + 1, int(inputs.size()), QFileInfo(inputs[i].path).fileName());
    }
    for (int i = 0; i < inputs.size(); ++i)
    {
        checkCancel(cancelled);
        if (hash(inputBytes(inputs[i].path)) != baselines[i])
            fail("作成中に入力画像が変更されました: " + QFileInfo(inputs[i].path).fileName());
    }
    auto result = builder.build();
    checkCancel(cancelled);
    if (result.getCatalog()->getPageCount() != size_t(inputs.size()))
        fail("作成したPDFのページ数が不正です。");
    return result;
}
catch (const pdf::PDFException& error)
{
    fail("画像からPDFを作成できません: " + error.getMessage());
}
} // namespace tatsu
