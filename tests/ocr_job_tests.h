#pragma once
#include <QJsonObject>
QJsonObject testOcrJobCleanup(const QString& output);
QJsonObject testLongWindowsPaths(const QString& fixtures, const QString& output);
QJsonObject testOcrResultValidation(const QString& fixtures, const QString& output);
QJsonObject testOcrWindowTeardown(const QString& fixtures, const QString& output);
