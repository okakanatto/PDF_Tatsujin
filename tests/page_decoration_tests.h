#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testDecorationLifecycle(const QString& fixtures, const QString& output);
QJsonObject testDecorationFailures(const QString& fixtures);
QJsonObject testDecorationUi(const QString& fixtures, const QString& output);
QJsonObject testDecorationCancel(const QString& fixtures);
QJsonObject testDecorationOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
