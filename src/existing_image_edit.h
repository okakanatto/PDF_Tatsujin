#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct ExistingImage
{
    int occurrence = 0;
    QByteArray resource;
    PDFObjectReference reference;
    QTransform matrix;
    QRectF physical;
    QSize pixels;
    int depth = 0;
};
enum class ExistingImageChange
{
    Geometry,
    Replace,
    Remove
};
QVector<ExistingImage> existingImages(const PDFDocument& document, int page,
                                      const std::function<bool()>& cancelled = {},
                                      bool includeForms = false);
PDFDocument editExistingImage(const PDFDocument& snapshot, int page, int occurrence,
                              ExistingImageChange change, QRectF physical = {},
                              const QImage& replacement = {},
                              const std::function<bool()>& cancelled = {},
                              bool includeForms = false);
} // namespace tatsu
