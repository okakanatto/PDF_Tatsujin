#pragma once
#include <QJsonObject>
#include <QString>

namespace tatsu
{
QJsonObject testImagePdfGeometry(const QString& output);
QJsonObject testImagePdfFailures(const QString& output);
QJsonObject testImagePdfWindow(const QString& fixtures, const QString& output);
QJsonObject testImagePdfCancelRetry(const QString& output);
QJsonObject testImagePdfOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
