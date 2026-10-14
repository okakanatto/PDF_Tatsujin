#pragma once
#include <QByteArray>
namespace tatsu
{
// Only for Qt's generated TrueType OCR subset; preserves every glyph outline.
QByteArray columnOcrFont(QByteArray font);
} // namespace tatsu
