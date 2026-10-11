#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testPageArrange(const QString& fixtures, const QString& output);
QJsonObject testPageMerge(const QString& fixtures, const QString& output);
QJsonObject testPageExports(const QString& fixtures, const QString& output);
QJsonObject testPageOrganizer(const QString& fixtures, const QString& output);
} // namespace tatsu
