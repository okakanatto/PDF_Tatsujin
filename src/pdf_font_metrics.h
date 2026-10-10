#pragma once
#include "pdffont.h"

namespace tatsu
{
// Compiled in the core DLL so the pinned library's private concrete font types
// remain private. The application imports this narrow, read-only bridge.
PDF4QTLIBCORESHARED_EXPORT double pdfGlyphAdvance(const pdf::PDFFontPointer& font, pdf::CID cid,
                                                  const QByteArray& baseFont,
                                                  const QByteArray& encoding);
} // namespace tatsu
