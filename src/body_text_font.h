#pragma once
#include "document.h"
#include "pdfdocumentbuilder.h"
#include <functional>

namespace tatsu
{
struct EmbeddedBodyFont
{
    PDFObject font;
    QVector<QByteArray> encodedLines;
};
// Generates only a permitted subset, and imports its font resources into the
// candidate builder. Does not copy an installed font into runtime assets.
EmbeddedBodyFont embedBodyTextFont(PDFDocumentBuilder& builder, const QString& family,
                                   const QString& text,
                                   const std::function<bool()>& cancelled = {});
} // namespace tatsu
