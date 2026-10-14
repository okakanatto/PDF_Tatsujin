#pragma once
#include "pdfdocument.h"
#include <functional>
#include <optional>

namespace tatsu
{
bool requiresRedactionFontSchemaCleanup(const pdf::PDFDocument& source,
                                        const pdf::PDFDictionary& declaration);
std::optional<pdf::PDFDictionary> canonicalRedactionStandardFont(const pdf::PDFDocument& source,
                                                                 const pdf::PDFObject& declaration);
void mergeRemovedDirectRedactionFonts(QVector<pdf::PDFObject>& target,
                                      const QVector<pdf::PDFObject>& declarations,
                                      const std::function<bool()>& cancelled);
std::optional<float> redactionStandardTextAdjustment(float from, float target, float size,
                                                     const std::function<bool()>& cancelled);
} // namespace tatsu
