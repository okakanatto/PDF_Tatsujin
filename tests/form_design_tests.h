#pragma once
#include <QJsonObject>
#include <QString>
namespace tatsu
{
QJsonObject testFormDesignLifecycle(const QString& fixtures, const QString& output);
QJsonObject testFormDesignFailures(const QString& fixtures, const QString& output);
QJsonObject testFormDesignUi(const QString& fixtures, const QString& output);
QJsonObject testFormDesignOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
