#pragma once
#include "redaction_content.h"

namespace tatsu
{
void rebuildSharedRedactionResources(const PDFDocument& source,
                                     const QMap<int, QVector<QRectF>>& rectangles,
                                     const std::set<pdf::PDFObjectReference>& deletedAnnotations,
                                     std::set<pdf::PDFObjectReference>& modifiedImages,
                                     std::set<pdf::PDFObjectReference>& removedDependencies,
                                     QVector<pdf::PDFObject>& directFonts,
                                     const std::set<pdf::PDFObjectReference>& forbiddenForms,
                                     const QMap<pdf::PDFObjectReference, QPainterPath>& formRegions,
                                     pdf::PDFDocumentBuilder& builder,
                                     const std::function<bool()>& cancelled,
                                     RedactionFontCvtUsage* cvtUsage = nullptr);
} // namespace tatsu
