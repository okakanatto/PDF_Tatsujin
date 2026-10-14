#include "certificate_crypto.h"
#include <openssl/x509v3.h>

namespace tatsu::certificate_detail
{
namespace
{
QString name(X509_NAME* value)
{
    std::unique_ptr<BIO, decltype(&BIO_free)> buffer(BIO_new(BIO_s_mem()), BIO_free);
    if (!buffer ||
        X509_NAME_print_ex(buffer.get(), value, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB) < 0)
        return {};
    char* text = nullptr;
    const auto size = BIO_get_mem_data(buffer.get(), &text);
    return QString::fromUtf8(text, size);
}
QDateTime date(const ASN1_TIME* value)
{
    tm time{};
    if (!ASN1_TIME_to_tm(value, &time))
        return {};
    return QDateTime(QDate(time.tm_year + 1900, time.tm_mon + 1, time.tm_mday),
                     QTime(time.tm_hour, time.tm_min, time.tm_sec), QTimeZone::UTC);
}
} // namespace
CertificateIdentity describeCertificate(X509* certificate)
{
    CertificateIdentity result;
    result.subject = name(X509_get_subject_name(certificate));
    result.issuer = name(X509_get_issuer_name(certificate));
    result.notBefore = date(X509_get0_notBefore(certificate));
    result.notAfter = date(X509_get0_notAfter(certificate));
    std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(
        ASN1_INTEGER_to_BN(X509_get_serialNumber(certificate), nullptr), BN_free);
    if (serial)
    {
        auto text = BN_bn2hex(serial.get());
        result.serial = QString::fromLatin1(text);
        OPENSSL_free(text);
    }
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(certificate, EVP_sha256(), bytes, &length))
        result.fingerprint = QByteArray(reinterpret_cast<char*>(bytes), length).toHex();
    auto key = X509_get0_pubkey(certificate);
    if (key)
        result.key =
            QString("%1 %2bit").arg(EVP_PKEY_get0_type_name(key)).arg(EVP_PKEY_get_bits(key));
    result.extendedPurpose = X509_get_ext_by_NID(certificate, NID_ext_key_usage, -1) >= 0;
    return result;
}
bool supportedPublicKey(EVP_PKEY* key)
{
    return key && ((EVP_PKEY_is_a(key, "RSA") && EVP_PKEY_get_bits(key) >= 2048) ||
                   (EVP_PKEY_is_a(key, "EC") && EVP_PKEY_get_bits(key) >= 256));
}
} // namespace tatsu::certificate_detail
