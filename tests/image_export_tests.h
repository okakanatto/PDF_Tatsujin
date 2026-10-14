#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testImageExportGeometry(const QString& fixtures, const QString& output);
QJsonObject testImageExportFailures(const QString& fixtures, const QString& output);
QJsonObject testImageExportUi(const QString& fixtures, const QString& output);
} // namespace tatsu
