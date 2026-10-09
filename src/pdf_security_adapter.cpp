#include "pdf_security_adapter.h"
#include "pdfsecurityhandler.h"

namespace tatsu
{
QByteArray sdkPreparedPassword(const QString& password)
{
    return pdf::PDFStandardOrPublicSecurityHandler::adjustPassword(password, 6);
}
} // namespace tatsu
