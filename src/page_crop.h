#pragma once
#include "document.h"

namespace tatsu
{
QRectF croppedPageBox(const PDFPage* page, QMarginsF millimeters);
PDFDocument cropPages(const PDFDocument& document, const QVector<int>& pages,
                      QMarginsF millimeters);
} // namespace tatsu
