#pragma once
#include "document.h"
#include "pdfdocumentbuilder.h"
#include <functional>
#include <set>

namespace tatsu
{
// Internal content transform, not a secure document export. The caller must
// separately sanitize annotations, fields, metadata, attachments and resources.
struct RedactedPageContent
{
    QByteArray bytes;
    pdf::PDFDictionary xobjects;
    pdf::PDFDictionary fonts;
    std::set<pdf::PDFObjectReference> modifiedImageSources;
    std::set<pdf::PDFObjectReference> removedResourceDependencies;
    int removedTextSegments = 0, modifiedImages = 0;
    int removedContentGroups = 0;
};
QPainterPath checkedRedactionRegions(const PDFDocument& document, int page,
                                     const QVector<QRectF>& regions);
RedactedPageContent redactPageContent(const PDFDocument& document, int page,
                                      const QVector<QRectF>& regions,
                                      pdf::PDFDocumentBuilder& privateBuilder,
                                      const std::function<bool()>& cancelled = {});
} // namespace tatsu
