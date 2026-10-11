#include "pdf_font_metrics.h"
#include "standard_font_metrics.h"
#include <limits>

namespace tatsu
{
double pdfGlyphAdvance(const pdf::PDFFontPointer& font, pdf::CID cid, const QByteArray& baseFont,
                       const QByteArray& encoding)
{
    if (const auto simple = dynamic_cast<const pdf::PDFSimpleFont*>(font.data()))
    {
        const auto width = simple->getGlyphAdvance(cid);
        if (width)
            return width;
        const auto standard = detail::standardFontGlyphWidth(baseFont, encoding, cid);
        return standard >= 0 ? standard : std::numeric_limits<double>::quiet_NaN();
    }
    if (const auto composite = dynamic_cast<const pdf::PDFType0Font*>(font.data()))
        return composite->getGlyphAdvance(cid);
    return std::numeric_limits<double>::quiet_NaN();
}
} // namespace tatsu
