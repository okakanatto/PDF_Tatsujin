#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testCertificateSigningFoundation(const QString& fixtures, const QString& output);
QJsonObject testCertificateSigningUi(const QString& fixtures, const QString& output);
QJsonObject testCertificateSigningAtomic(const QString& fixtures, const QString& output);
QJsonObject testCertificateSigningOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
