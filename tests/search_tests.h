#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testSearchNavigation(const QString& fixtures, const QString& output);
QJsonObject testSearchGeneration(const QString& fixtures, const QString& output);
QJsonObject testSearchInput(const QString& fixtures, const QString& output);
} // namespace tatsu
