#pragma once
#include <QJsonObject>
namespace tatsu
{
QJsonObject testVerticalOcrUi(const QString& fixtures, const QString& output);
QJsonObject testVerticalOcrFailure(const QString& fixtures, const QString& output);
QJsonObject testVerticalFontMetrics(const QString& fixtures);
} // namespace tatsu
