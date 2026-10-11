#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct EncryptionOptions
{
    QString userPassword, ownerPassword;
    bool print = true, copy = true, forms = true, annotations = false, assemble = false,
         modify = false;
};
QString preparePdfPassword(const QString& password);
PDFDocument protectPdf(const PDFDocument& document, const EncryptionOptions& options);
void exportProtectedPdf(const PDFDocument& document, const EncryptionOptions& options,
                        const QString& destination, const std::function<bool()>& cancelled = {},
                        const std::function<void(QString)>& progress = {});
} // namespace tatsu
