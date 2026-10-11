#pragma once
#include "form_fields.h"
#include "pdfdocumentbuilder.h"

namespace tatsu
{
// Complete embedded font used by the editable default appearance and each
// appearance stream. CIDs represent Unicode scalars, independently of GIDs.
PDFObjectReference embedFormFont(PDFDocumentBuilder& builder);
bool isFormFont(const PDFDocument& document, PDFObjectReference reference);
void validateFormFontText(const QString& text);
PDFObjectReference formFontAppearance(PDFDocumentBuilder& builder, PDFObjectReference font,
                                      QSizeF dimensions, const FormField& field,
                                      const QStringList& values, double unit, int rotation = 0);
} // namespace tatsu
