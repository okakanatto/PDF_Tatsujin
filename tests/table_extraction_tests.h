#pragma once
#include <QJsonObject>
namespace tatsu
{
QJsonObject testTableExtraction(const QString& fixtures, const QString& output);
QJsonObject testTableExtractionUi(const QString& fixtures, const QString& output);
QJsonObject testTableOfficeInterop(const QString& output);
} // namespace tatsu
