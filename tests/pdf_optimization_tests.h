#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testOptimizationLifecycle(const QString& fixtures, const QString& output);
QJsonObject testOptimizationFailures(const QString& fixtures);
QJsonObject testOptimizationUi(const QString& fixtures, const QString& output);
QJsonObject testOptimizationOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
