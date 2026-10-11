#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testFormDataLifecycle(const QString& fixtures, const QString& output);
QJsonObject testFormDataFailures(const QString& fixtures, const QString& output);
QJsonObject testFormDataUi(const QString& fixtures, const QString& output);
QJsonObject testFormDataOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
