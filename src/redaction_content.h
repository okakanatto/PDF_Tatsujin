#pragma once
#include "document.h"
#include "pdfdocumentbuilder.h"
#include <functional>
#include <set>

namespace tatsu
{
class RedactionResourceUsage;
class RedactionFontCvtUsage;
// Internal content transform, not a secure document export. The caller must
// separately sanitize annotations, fields, metadata, attachments and resources.
struct RedactedPageContent
{
    QByteArray bytes;
    pdf::PDFDictionary xobjects;
    pdf::PDFDictionary fonts;
    pdf::PDFDictionary colorSpaces, graphicsStates;
    std::set<pdf::PDFObjectReference> modifiedImageSources;
    std::set<pdf::PDFObjectReference> removedResourceDependencies;
    QVector<pdf::PDFObject> removedDirectFontDeclarations;
    std::set<pdf::PDFObjectReference> removedFormSources;
    QMap<pdf::PDFObjectReference, QPainterPath> redactedFormRegions;
    int removedTextSegments = 0, modifiedImages = 0;
    int removedContentGroups = 0;
};
QPainterPath checkedRedactionRegions(const PDFDocument& document, int page,
                                     const QVector<QRectF>& regions);
RedactedPageContent redactPageContent(const PDFDocument& document, int page,
                                      const QVector<QRectF>& regions,
                                      pdf::PDFDocumentBuilder& privateBuilder,
                                      const std::function<bool()>& cancelled = {},
                                      const RedactionResourceUsage* shared = nullptr,
                                      RedactionFontCvtUsage* cvtUsage = nullptr);
// Recode a disjoint annotation appearance using the same bounded form profile.
RedactedPageContent reencodeRedactionAppearance(const PDFDocument& document, int page,
                                                const pdf::PDFObject& appearance,
                                                pdf::PDFDocumentBuilder& privateBuilder,
                                                const RedactionResourceUsage& shared,
                                                const std::function<bool()>& cancelled = {},
                                                RedactionFontCvtUsage* cvtUsage = nullptr);
} // namespace tatsu
