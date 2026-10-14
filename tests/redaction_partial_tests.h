#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testRedactionPartial(const QString& fixtures, const QString& output);
QJsonObject testRedactionPartialFailures(const QString& fixtures, const QString& output);
QJsonObject testRedactionPartialUi(const QString& fixtures, const QString& output);
} // namespace tatsu
