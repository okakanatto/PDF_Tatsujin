#pragma once
#include "document.h"
#include "pdfdocumentbuilder.h"
namespace tatsu
{
// The pinned upstream highlight generator omits /Length. Repair generated
// streams, including superseded appearances, before publishing a candidate.
void completeAnnotationStreams(pdf::PDFDocumentBuilder& builder);
void updateOwnedAnnotationAppearance(pdf::PDFDocumentBuilder& builder,
                                     PDFObjectReference reference);
void removeOwnedAnnotation(pdf::PDFDocumentBuilder& builder, PDFObjectReference page,
                           PDFObjectReference reference);
Signature putAnnotation(Document& document, int page, OverlayKind kind, QRectF rectangle,
                        const QString& contents, QColor color, double width,
                        QPolygonF geometry = {}, PDFObjectReference old = {});
QVector<PDFObjectReference>
putHighlights(Document& document, const QMap<int, QVector<QRectF>>& selections, QColor color);
} // namespace tatsu
