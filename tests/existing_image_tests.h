#pragma once
#include <QtCore>

namespace tatsu
{
QJsonObject testExistingImageCore(const QString& fixtures, const QString& output);
QJsonObject testExistingImageRejections(const QString& fixtures, const QString& output);
QJsonObject testExistingImageUi(const QString& fixtures, const QString& output);
QJsonObject testExistingImageUiModes(const QString& fixtures, const QString& output);
} // namespace tatsu
