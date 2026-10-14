#pragma once
#include <QJsonObject>
namespace tatsu
{
enum class VerticalProfile
{
    Standard,
    Unit,
    Rotate90,
    Rotate180,
    Rotate270
};
QJsonObject testVerticalOcrUi(const QString& fixtures, const QString& output);
QJsonObject testVerticalOcrUnitUi(const QString& fixtures, const QString& output);
QJsonObject testVerticalOcrRotationUi(const QString& fixtures, const QString& output,
                                      VerticalProfile profile = VerticalProfile::Rotate90);
QJsonObject testVerticalGeometryCancel(const QString& fixtures, VerticalProfile profile);
QJsonObject testVerticalOcrFailure(const QString& fixtures, const QString& output);
QJsonObject testVerticalFontMetrics(const QString& fixtures);
} // namespace tatsu
