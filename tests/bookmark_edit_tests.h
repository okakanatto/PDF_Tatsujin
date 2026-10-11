#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testBookmarkLifecycle(const QString& fixtures, const QString& output);
QJsonObject testBookmarkFailures(const QString& fixtures);
QJsonObject testBookmarkEditUi(const QString& fixtures, const QString& output);
QJsonObject testBookmarkCancel(const QString& fixtures);
} // namespace tatsu
