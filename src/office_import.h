#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
enum class OfficeKind
{
    Document,
    Spreadsheet,
    Presentation
};
struct OfficeInput
{
    QByteArray bytes;
    OfficeKind kind = OfficeKind::Document;
};
QString officeConverterPath();
OfficeInput validatedOffice(const QString& path, const std::function<bool()>& cancelled = {});
QByteArray validatedDocx(const QString& path, const std::function<bool()>& cancelled = {});
PDFDocument importDocx(const QString& path, const QString& converter,
                       const std::function<bool()>& cancelled = {},
                       bool suppressAsianSpacing = false);
PDFDocument importOffice(const QString& path, const QString& converter,
                         const std::function<bool()>& cancelled = {},
                         bool suppressAsianSpacing = false);
} // namespace tatsu
