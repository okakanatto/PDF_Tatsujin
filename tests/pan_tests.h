#pragma once
#include <QJsonObject>
#include <QString>

namespace tatsu
{
QJsonObject testPanNavigation(const QString& fixtures, const QString& output);
QJsonObject testPanInput(const QString& fixtures, const QString& output);
QJsonObject testPanLifecycle(const QString& fixtures, const QString& output);
} // namespace tatsu
