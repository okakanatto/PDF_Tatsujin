#include "ocr_preprocess.h"
#include <algorithm>
#include <cmath>

namespace tatsu
{
std::optional<QRectF> axisAlignedRectangle(const QPainterPath& path)
{
    if (path.elementCount() != 5 || path.elementAt(0).type != QPainterPath::MoveToElement ||
        QPointF(path.elementAt(0)) != QPointF(path.elementAt(4)))
        return {};
    const auto bounds = path.boundingRect();
    if (!bounds.isValid() || !std::isfinite(bounds.left()) || !std::isfinite(bounds.top()) ||
        !std::isfinite(bounds.right()) || !std::isfinite(bounds.bottom()))
        return {};
    QVector<QPointF> corners{bounds.topLeft(), bounds.topRight(), bounds.bottomRight(),
                             bounds.bottomLeft()};
    for (int index = 0; index < 4; ++index)
    {
        const auto element = path.elementAt(index);
        if ((index && element.type != QPainterPath::LineToElement) ||
            !corners.removeOne(QPointF(element)))
            return {};
    }
    return bounds;
}
int omitSolidBlackOcrBlocks(QImage& image, const QVector<QRectF>& rectangles)
{
    if (image.format() != QImage::Format_Grayscale8)
        return 0;
    qint64 budget = qint64(image.width()) * image.height() * 2;
    int omitted = 0;
    for (const auto& box : rectangles.mid(0, 1000))
    {
        if (!box.isValid() || !QRectF(image.rect()).contains(box))
            continue;
        // Keep antialias boundary pixels intact; require a meaningful interior.
        const int left = int(std::ceil(box.left())) + 1, top = int(std::ceil(box.top())) + 1;
        const int right = int(std::floor(box.right())) - 1,
                  bottom = int(std::floor(box.bottom())) - 1;
        if (right - left < 8 || bottom - top < 8)
            continue;
        const qint64 pixels = qint64(right - left) * (bottom - top);
        if (pixels > budget)
            continue;
        budget -= pixels;
        bool black = true;
        for (int y = top; y < bottom && black; ++y)
            black = std::all_of(image.constScanLine(y) + left, image.constScanLine(y) + right,
                                [](uchar value) { return value == 0; });
        if (!black)
            continue;
        for (int y = top; y < bottom; ++y)
            std::fill(image.scanLine(y) + left, image.scanLine(y) + right, uchar(255));
        ++omitted;
    }
    return omitted;
}
} // namespace tatsu
