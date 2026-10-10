#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testExistingTextCore(const QString& fixtures, const QString& output);
QJsonObject testExistingTextRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingTextUi(const QString& fixtures, const QString& output);
QJsonObject testExistingTextUiRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingTextFonts(const QString& fixtures, const QString& output);
QJsonObject testExistingTextFontUi(const QString& fixtures, const QString& output);
} // namespace tatsu
