#pragma once
#include "existing_image_edit.h"

namespace tatsu
{
QVector<ExistingImage> formImageDrawings(const PDFDocument& document, int page,
                                         const std::function<bool()>& cancelled);
PDFDocument editFormImageDrawing(const PDFDocument& document, int page, int occurrence,
                                 ExistingImageChange change, QRectF physical,
                                 const QImage& replacement, const std::function<bool()>& cancelled);
} // namespace tatsu
