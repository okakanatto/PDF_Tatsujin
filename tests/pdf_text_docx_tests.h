#pragma once
#include <QJsonObject>
namespace tatsu
{
QJsonObject testWordTextCore(const QString& fixtures, const QString& output);
QJsonObject testWordTextFailures(const QString& fixtures, const QString& output);
QJsonObject testWordTextUi(const QString& fixtures, const QString& output);
QJsonObject testWordTextOffice(const QString& fixtures, const QString& output);
} // namespace tatsu
