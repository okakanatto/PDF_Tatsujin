#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testUnprotectedLifecycle(const QString& fixtures, const QString& output);
QJsonObject testUnprotectedFailures(const QString& fixtures, const QString& output);
QJsonObject testUnprotectedUi(const QString& fixtures, const QString& output);
QJsonObject testUnprotectedOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
