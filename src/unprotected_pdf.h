#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
PDFDocument unprotectPdf(const PDFDocument& document, const QString& ownerPassword,
                         const std::function<bool()>& cancelled = {});
QByteArray exportUnprotectedPdf(const PDFDocument& document, const QString& ownerPassword,
                                const QString& destination,
                                const std::function<bool()>& cancelled = {},
                                const std::function<void(QString)>& progress = {});
} // namespace tatsu
