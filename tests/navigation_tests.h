#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testNavigationBookmarks(const QString& fixtures, const QString& output);
QJsonObject testNavigationLinks(const QString& fixtures, const QString& output);
QJsonObject testNavigationLifecycle(const QString& fixtures, const QString& output);
} // namespace tatsu
