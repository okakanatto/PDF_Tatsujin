#pragma once
#include <QtCore>

namespace tatsu
{
QJsonObject testFontRoundtrip(const QString& fixtures, const QString& output);
QJsonObject testFontFailures(const QString& fixtures, const QString& output);
QJsonObject testFontUi(const QString& fixtures, const QString& output);
} // namespace tatsu
