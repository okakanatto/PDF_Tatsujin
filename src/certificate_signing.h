#pragma once
#include "certificate_verification.h"
#include "document.h"

namespace tatsu
{
struct SignedPdfCandidate
{
    QByteArray bytes;
    CertificateIdentity certificate;
    QString field;
};
CertificateIdentity inspectSigningCertificate(const QByteArray& pkcs12, const QString& password);
SignedPdfCandidate prepareSignedPdf(const PDFDocument& document, const QByteArray& pkcs12,
                                    const QString& password, const QString& reason,
                                    const std::function<bool()>& cancelled = {});
QByteArray exportSignedPdf(const PDFDocument& document, const QByteArray& pkcs12,
                           const QString& password, const QString& reason,
                           const QString& destination, const std::function<bool()>& cancelled = {},
                           const std::function<void(QString)>& progress = {},
                           const std::function<void()>& validateInputs = {});
} // namespace tatsu
