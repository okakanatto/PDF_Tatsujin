#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testPageAxes(const QString& fixtures);
QJsonObject testPageGeometryRender(const QString& fixtures, const QString& output);
} // namespace tatsu
