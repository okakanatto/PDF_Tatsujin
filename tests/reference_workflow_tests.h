#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testCompactReferences(const QString& fixtures, const QString& output);
QJsonObject testRepeatSearchReference(const QString& fixtures, const QString& output);
QJsonObject testSearchResultResize(const QString& fixtures, const QString& output);
QJsonObject testReferenceDuringOcr(const QString& fixtures, const QString& output);
QJsonObject testReferenceContexts(const QString& fixtures, const QString& output);
} // namespace tatsu
