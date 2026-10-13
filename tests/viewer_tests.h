#pragma once
#include <QJsonObject>
#include <QString>

namespace tatsu
{
QJsonObject testViewerNavigation(const QString& fixtures, const QString& output);
QJsonObject testViewerCoordinates(const QString& fixtures, const QString& output);
QJsonObject testViewerSignature(const QString& fixtures, const QString& output);
QJsonObject testPagePreviews(const QString& fixtures, const QString& output);
} // namespace tatsu
