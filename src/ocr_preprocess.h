#pragma once
#include <QImage>
#include <QPainterPath>
#include <QVector>
#include <optional>

namespace tatsu
{
std::optional<QRectF> axisAlignedRectangle(const QPainterPath& path);
// Only the OCR raster changes. Painted PDF content and source pixels are retained.
int omitSolidBlackOcrBlocks(QImage& grayscale, const QVector<QRectF>& rectangles);
} // namespace tatsu
