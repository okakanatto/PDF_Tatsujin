#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct ImagePdfInput
{
    QString path;
    QByteArray expectedHash;
};
struct ImagePdfOptions
{
    enum class PageMode
    {
        AutoA4,
        ImageSize
    };
    PageMode pageMode = PageMode::AutoA4;
    double dpi = 150;
};
struct ImagePdfLayout
{
    QSizeF page;
    QRectF image;
};
ImagePdfLayout imagePdfLayout(QSize pixels, const ImagePdfOptions& options);
QImage imagePdfPreview(const QString& path, QByteArray* hash = nullptr);
PDFDocument createImagePdf(const QVector<ImagePdfInput>& inputs, const ImagePdfOptions& options,
                           const std::function<bool()>& cancelled = {},
                           const std::function<void(int, int, const QString&)>& progress = {});
} // namespace tatsu
