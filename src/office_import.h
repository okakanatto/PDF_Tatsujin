#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
QString officeConverterPath();
QByteArray validatedDocx(const QString& path, const std::function<bool()>& cancelled = {});
PDFDocument importDocx(const QString& path, const QString& converter,
                       const std::function<bool()>& cancelled = {},
                       bool suppressAsianSpacing = false);
} // namespace tatsu
