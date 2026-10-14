#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testLinkLifecycle(const QString& fixtures, const QString& output);
QJsonObject testLinkFailures(const QString& fixtures);
QJsonObject testLinkUi(const QString& fixtures, const QString& output);
QJsonObject testLinkOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
