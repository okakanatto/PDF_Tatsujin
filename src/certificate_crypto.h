#pragma once
#include "certificate_verification.h"
#include <openssl/x509.h>

namespace tatsu::certificate_detail
{
CertificateIdentity describeCertificate(X509* certificate);
bool supportedPublicKey(EVP_PKEY* key);
} // namespace tatsu::certificate_detail
