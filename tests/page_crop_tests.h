#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testPageCropGeometry(const QString& fixtures, const QString& output);
QJsonObject testPageCropFailures(const QString& fixtures);
QJsonObject testPageCropUi(const QString& fixtures, const QString& output);
} // namespace tatsu
