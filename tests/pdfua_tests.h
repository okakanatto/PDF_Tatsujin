#pragma once
#include <QJsonObject>
namespace tatsu
{
QJsonObject testPdfuaEngine(const QString& fixtures, const QString& output);
QJsonObject testPdfuaFailures(const QString& fixtures, const QString& output);
QJsonObject testPdfuaUi(const QString& fixtures, const QString& output);
} // namespace tatsu
