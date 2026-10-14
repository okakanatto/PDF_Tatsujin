#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testFormValues(const QString& fixtures, const QString& output);
QJsonObject testFormInput(const QString& fixtures, const QString& output);
QJsonObject testFormKeyboard(const QString& fixtures, const QString& output);
QJsonObject testWindowPanelLifetime(const QString& fixtures, const QString& output);
} // namespace tatsu
