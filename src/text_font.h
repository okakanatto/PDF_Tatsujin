#pragma once
#include <QtGui>

namespace tatsu
{
QStringList textFontFamilies();
bool allowsEditableFontEmbedding(const QByteArray& os2Table);
QFont textFont(const QString& family);
} // namespace tatsu
