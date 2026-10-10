#pragma once
#include <QtCore>
#include <functional>

namespace tatsu
{
enum class SignatureIntegrity
{
    Unsigned,
    Unchanged,
    Invalid,
    Unsupported
};
enum class CertificateChain
{
    NotChecked,
    LocalTrusted,
    Untrusted,
    Expired,
    NotYetValid,
    Invalid
};
struct CertificateIdentity
{
    QString subject, issuer, serial, fingerprint, key;
    QDateTime notBefore, notAfter;
    bool extendedPurpose = false;
};
struct CertificateSignature
{
    QString field, format, detail, digest;
    SignatureIntegrity integrity = SignatureIntegrity::Unsupported;
    CertificateChain chain = CertificateChain::NotChecked;
    bool entireFile = false;
    qint64 signedBytes = 0, unsignedTail = 0;
    CertificateIdentity certificate;
};
struct CertificateVerification
{
    QByteArray fileHash;
    QDateTime checkedAt;
    QVector<CertificateSignature> signatures;
};
struct CertificateTrust
{
    bool windowsRoots = true;
    // Used by synthetic tests only. Never persists trust or imports into the OS.
    QVector<QByteArray> additionalDer;
};
QString signatureIntegrityText(SignatureIntegrity value);
QString certificateChainText(CertificateChain value);
CertificateVerification verifyCertificateSignatures(const QString& path,
                                                    const QByteArray& expectedHash,
                                                    const CertificateTrust& trust = {},
                                                    const std::function<bool()>& cancel = {});
} // namespace tatsu
