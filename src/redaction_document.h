#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
// Experimental private candidate, unavailable in the product UI. Its supported
// subset must pass independent security and preservation tests before export.
struct RedactionCandidate
{
    PDFDocument document;
    int textSegments = 0, images = 0, fieldGroups = 0, annotations = 0;
};
RedactionCandidate prepareRedactionCandidate(const PDFDocument& document,
                                             const QMap<int, QVector<QRectF>>& regions,
                                             const std::function<bool()>& cancelled = {});
} // namespace tatsu
