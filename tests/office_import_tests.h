#pragma once
#include <QJsonObject>
namespace tatsu
{
QJsonObject testOfficeImport(const QString& fixtures, const QString& output);
QJsonObject testOfficeImportUi(const QString& fixtures, const QString& output);
QJsonObject testOwnedProcess(const QString& output);
QJsonObject testOfficeImportWindow(const QString& fixtures, const QString& output);
} // namespace tatsu
