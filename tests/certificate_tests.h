#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testCertificateCases(const QString& fixtures, const QString& output);
QJsonObject testCertificateGuards(const QString& fixtures, const QString& output);
QJsonObject testCertificateUi(const QString& fixtures, const QString& output);
} // namespace tatsu
