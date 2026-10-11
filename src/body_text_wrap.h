#pragma once
#include <QtCore>
#include <functional>

namespace tatsu
{
struct BodyGlyphExtent
{
    double advance = 0, left = 0, right = 0;
};
QString wrapBodyText(const QString& text, double width,
                     const std::function<QVector<BodyGlyphExtent>(const QString&)>& measure,
                     const std::function<bool()>& cancelled);
} // namespace tatsu
