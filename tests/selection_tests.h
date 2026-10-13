#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testSelectionRanges(const QString& fixtures, const QString& output);
QJsonObject testSelectionScroll(const QString& fixtures, const QString& output);
QJsonObject testSelectionLifecycle(const QString& fixtures, const QString& output);
QJsonObject testSelectionOcr(const QString& pdfPath, const QString& output);
QJsonObject testSelectionWords(const QString& fixtures, const QString& output);
QJsonObject testSelectionWordBoundaries(const QString& fixtures, const QString& output);
QJsonObject testSelectionWordLifecycle(const QString& fixtures, const QString& output);
} // namespace tatsu
