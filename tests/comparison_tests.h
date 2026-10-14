#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testComparisonLifecycle(const QString& fixtures, const QString& output);
QJsonObject testComparisonFailures(const QString& fixtures, const QString& output);
QJsonObject testComparisonUi(const QString& fixtures, const QString& output);
QJsonObject testComparisonOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
