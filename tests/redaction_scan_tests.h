#pragma once
#include <QtCore>

namespace tatsu
{
QJsonObject testRedactionScanOcr(const QString& fixtures, const QString& output);
QJsonObject testRedactionOcrMasks();
} // namespace tatsu
