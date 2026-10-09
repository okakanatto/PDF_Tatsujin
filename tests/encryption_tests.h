#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testEncryptionPasswords(const QString& output);
QJsonObject testEncryptionLifecycle(const QString& fixtures, const QString& output);
QJsonObject testEncryptionFailures(const QString& fixtures, const QString& output);
QJsonObject testEncryptionUi(const QString& fixtures, const QString& output);
QJsonObject testEncryptionOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
