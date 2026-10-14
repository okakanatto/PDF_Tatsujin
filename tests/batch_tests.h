#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testBatchLifecycle(const QString& fixtures, const QString& output);
QJsonObject testBatchFailures(const QString& fixtures, const QString& output);
QJsonObject testBatchUi(const QString& fixtures, const QString& output);
QJsonObject testBatchOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
