#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testReadingPageInput(const QString& fixtures, const QString& output);
QJsonObject testReadingLayout(const QString& fixtures, const QString& output);
QJsonObject testReadingOcrCancel(const QString& fixtures, const QString& output);
} // namespace tatsu
