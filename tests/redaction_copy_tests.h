#pragma once
#include <QtCore>

namespace tatsu
{
QJsonObject testRedactionCopyFoundation(const QString& fixtures, const QString& output);
QJsonObject testRedactionCopyAtomic(const QString& fixtures, const QString& output);
QJsonObject testRedactionCopyUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionCopyFontUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionCopyGeometryUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionCopyOcr(const QString& fixtures, const QString& output);
} // namespace tatsu
