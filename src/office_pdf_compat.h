#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
QByteArray splitOfficeClosingAdjustments(const QByteArray& content);
PDFDocument normalizeOfficeClosingAdjustments(const PDFDocument& document,
                                              const std::function<bool()>& cancelled = {});
} // namespace tatsu
