#pragma once
#include "pdffont.h"
#include <QPointF>

namespace tatsu
{
// Private concrete font; the upstream public API and horizontal fonts stay intact.
class VerticalCidFont final : public pdf::PDFType0Font
{
public:
    using pdf::PDFType0Font::PDFType0Font;
    void configureOrigins(pdf::PDFReal defaultY, pdf::PDFReal defaultWidth,
                          std::unordered_map<pdf::CID, pdf::PDFReal> horizontalWidths,
                          std::unordered_map<pdf::CID, QPointF> origins)
    {
        originY = defaultY;
        horizontalWidth = defaultWidth;
        widths = std::move(horizontalWidths);
        positions = std::move(origins);
    }
    QPointF origin(pdf::CID cid) const
    {
        if (const auto found = positions.find(cid); found != positions.end())
            return found->second;
        const auto found = widths.find(cid);
        return {(found != widths.end() ? found->second : horizontalWidth) / 2, originY};
    }

private:
    pdf::PDFReal originY = 880;
    pdf::PDFReal horizontalWidth = 1000;
    std::unordered_map<pdf::CID, pdf::PDFReal> widths;
    std::unordered_map<pdf::CID, QPointF> positions;
};
} // namespace tatsu
