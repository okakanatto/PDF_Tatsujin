#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testPdfaEngine(const QString& fixtures, const QString& output);
QJsonObject testPdfaFailures(const QString& fixtures, const QString& output);
QJsonObject testPdfaUi(const QString& fixtures, const QString& output);
} // namespace tatsu
