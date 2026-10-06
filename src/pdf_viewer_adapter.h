#pragma once
#include "pdfglobal.h"
#include "pdfwidgetsglobal.h"
#include <vector>

namespace pdf
{
class PDFDrawWidgetProxy;
}
namespace tatsu
{
// Built inside Pdf4QtLibWidgets: its compiler is intentionally not exported.
PDF4QTLIBWIDGETSSHARED_EXPORT void prepareReadingPages(pdf::PDFDrawWidgetProxy* proxy,
                                                       const std::vector<pdf::PDFInteger>& visible,
                                                       int direction);
} // namespace tatsu
