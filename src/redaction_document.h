#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
// Prepare an isolated candidate for the supported subset. Export separately
// validates and publishes it without modifying the source document or history.
struct RedactionCandidate
{
    PDFDocument document;
    int textSegments = 0, images = 0, fieldGroups = 0, annotations = 0;
    int contentGroups = 0;
};
RedactionCandidate prepareRedactionCandidate(const PDFDocument& document,
                                             const QMap<int, QVector<QRectF>>& regions,
                                             const std::function<bool()>& cancelled = {});
} // namespace tatsu
