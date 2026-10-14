#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testLayoutTransitions(const QString& fixtures, const QString& output);
QJsonObject testResizeNavigationInput(const QString& fixtures, const QString& output);
QJsonObject testAutomaticFit(const QString& fixtures, const QString& output);
} // namespace tatsu
