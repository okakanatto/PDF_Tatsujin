#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testWritingRoundtrip(const QString& fixtures, const QString& output);
QJsonObject testWritingCoordinates(const QString& fixtures, const QString& output);
QJsonObject testWritingUi(const QString& fixtures, const QString& output);
QJsonObject testSignatureLibrary(const QString& fixtures, const QString& output);
} // namespace tatsu
