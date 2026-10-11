#include "certificate_verification.h"
#include "certificate_crypto.h"
#include "document.h"
#include "pdfcertificatestore.h"
#include "pdfdocumentreader.h"
#include "pdfsecurityhandler.h"
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <set>
#include <windows.h>

// The certificate store API requires Windows types to be declared first.
#include <wincrypt.h>

namespace tatsu
{
namespace
{
using namespace pdf;
template <typename T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
constexpr qint64 maxFile = 256 * 1024 * 1024, maxCms = 1024 * 1024;
void cancelled(const std::function<bool()>& cancel)
{
    if (cancel && cancel())
        fail("証明書署名の検証を中止しました。");
}
bool white(char c)
{
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
void addDer(X509_STORE* store, const QByteArray& der)
{
    const auto* data = reinterpret_cast<const unsigned char*>(der.constData());
    Owned<X509, X509_free> cert(d2i_X509(nullptr, &data, der.size()), X509_free);
    if (cert && data == reinterpret_cast<const unsigned char*>(der.constData() + der.size()))
        X509_STORE_add_cert(store, cert.get());
    ERR_clear_error();
}
Owned<X509_STORE, X509_STORE_free> roots(const CertificateTrust& trust,
                                         const std::function<bool()>& cancel)
{
    Owned<X509_STORE, X509_STORE_free> store(X509_STORE_new(), X509_STORE_free);
    if (!store)
        fail("証明書ストアを作成できません。");
    if (trust.windowsRoots)
    {
        auto windows = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG |
                                         CERT_STORE_OPEN_EXISTING_FLAG,
                                     L"ROOT");
        if (!windows)
            fail("WindowsのROOT証明書を読み取れません。");
        PCCERT_CONTEXT current = nullptr;
        while ((current = CertEnumCertificatesInStore(windows, current)))
        {
            if (cancel && cancel())
            {
                CertFreeCertificateContext(current);
                CertCloseStore(windows, 0);
                cancelled(cancel);
            }
            addDer(store.get(), QByteArray(reinterpret_cast<const char*>(current->pbCertEncoded),
                                           current->cbCertEncoded));
        }
        CertCloseStore(windows, 0);
    }
    for (const auto& der : trust.additionalDer)
    {
        cancelled(cancel);
        addDer(store.get(), der);
    }
    return store;
}
CertificateChain checkChain(X509* signer, STACK_OF(X509) * certificates, X509_STORE* store)
{
    Owned<X509_STORE_CTX, X509_STORE_CTX_free> context(X509_STORE_CTX_new(), X509_STORE_CTX_free);
    if (!context || !X509_STORE_CTX_init(context.get(), store, signer, certificates))
        return CertificateChain::Invalid;
    // Chain integrity and current validity only. No PDF-specific purpose or revocation claim.
    X509_VERIFY_PARAM_set_auth_level(X509_STORE_CTX_get0_param(context.get()), 2);
    if (X509_verify_cert(context.get()) == 1)
        return CertificateChain::LocalTrusted;
    switch (X509_STORE_CTX_get_error(context.get()))
    {
    case X509_V_ERR_CERT_HAS_EXPIRED:
        return CertificateChain::Expired;
    case X509_V_ERR_CERT_NOT_YET_VALID:
        return CertificateChain::NotYetValid;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        return CertificateChain::Untrusted;
    default:
        return CertificateChain::Invalid;
    }
}
struct Ranges
{
    qint64 first, second, length;
    QByteArray contents;
};
Ranges ranges(const PDFDocument& doc, const PDFDictionary& dictionary, const QByteArray& source)
{
    const auto range = doc.getObject(dictionary.get("ByteRange"));
    if (!range.isArray() || range.getArray()->getCount() != 4)
        fail("ByteRangeが整数4個ではありません。");
    qint64 values[4];
    for (int i = 0; i < 4; ++i)
    {
        const auto& value = range.getArray()->getItem(i);
        if (!value.isInt())
            fail("ByteRangeに整数でない値があります。");
        values[i] = value.getInteger();
    }
    const auto [begin, first, second, length] =
        std::tuple{values[0], values[1], values[2], values[3]};
    if (begin != 0 || first <= 0 || first > source.size() || second <= first ||
        second >= source.size() || length <= 0 || length > source.size() - second ||
        second - first > 2 * maxCms + 65536)
        fail("ByteRangeの順序または境界が不正です。");
    const auto contents = doc.getObject(dictionary.get("Contents"));
    if (!contents.isString() || contents.getString().isEmpty() ||
        contents.getString().size() > maxCms)
        fail("署名Contentsが不正または上限を超えています。");
    auto gap = source.mid(first, second - first);
    if (!gap.startsWith('<') || gap.startsWith("<<") || !gap.endsWith('>'))
        fail("署名対象外の範囲がContentsの16進文字列ではありません。");
    QByteArray hex;
    for (qsizetype i = 1; i + 1 < gap.size(); ++i)
    {
        const char c = gap[i];
        if (white(c))
            continue;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            fail("Contentsの16進文字列が不正です。");
        hex += c;
    }
    if ((hex.size() & 1) || QByteArray::fromHex(hex) != contents.getString())
        fail("ByteRangeの除外範囲とContentsが一致しません。");
    auto prefix = source.left(first).trimmed();
    if (!prefix.endsWith("/Contents"))
        fail("ByteRangeの除外範囲をContentsに結び付けられません。");
    return {first, second, length, contents.getString()};
}
CertificateSignature verifyOne(const PDFDocument& doc, const PDFObject& object,
                               const QString& field, const QByteArray& source, X509_STORE* store,
                               const std::function<bool()>& cancel)
{
    CertificateSignature row;
    row.field = field;
    const auto value = doc.getObject(object);
    if (value.isNull())
    {
        row.integrity = SignatureIntegrity::Unsigned;
        row.detail = "空の証明書署名欄です。";
        return row;
    }
    try
    {
        if (!value.isDictionary())
            fail("署名辞書が不正です。");
        const auto& dictionary = *value.getDictionary();
        auto format = doc.getObject(dictionary.get("SubFilter"));
        row.format = format.isName() ? QString::fromLatin1(format.getString()) : QString();
        if (row.format != "adbe.pkcs7.detached" && row.format != "ETSI.CAdES.detached")
        {
            row.detail = "この署名形式の検証は未対応です。";
            return row;
        }
        row.integrity = SignatureIntegrity::Invalid;
        const auto bounds = ranges(doc, dictionary, source);
        row.signedBytes = bounds.first + bounds.length;
        row.unsignedTail = source.size() - bounds.second - bounds.length;
        row.entireFile = row.unsignedTail == 0;
        const auto* cursor = reinterpret_cast<const unsigned char*>(bounds.contents.constData());
        Owned<CMS_ContentInfo, CMS_ContentInfo_free> cms(
            d2i_CMS_ContentInfo(nullptr, &cursor, bounds.contents.size()), CMS_ContentInfo_free);
        if (!cms || !CMS_is_detached(cms.get()) ||
            OBJ_obj2nid(CMS_get0_type(cms.get())) != NID_pkcs7_signed)
            fail("分離CMS署名を読み取れません。");
        const auto* end = reinterpret_cast<const unsigned char*>(bounds.contents.constData() +
                                                                 bounds.contents.size());
        while (cursor < end)
            if (*cursor++ != 0)
                fail("CMS署名の後に不正なデータがあります。");
        auto signers = CMS_get0_SignerInfos(cms.get());
        if (!signers || sk_CMS_SignerInfo_num(signers) != 1)
        {
            row.integrity = SignatureIntegrity::Unsupported;
            row.detail = "CMS内の複数署名者または署名者なしは未対応です。";
            return row;
        }
        X509_ALGOR* algorithm = nullptr;
        CMS_SignerInfo_get0_algs(sk_CMS_SignerInfo_value(signers, 0), nullptr, nullptr, &algorithm,
                                 nullptr);
        const auto nid = algorithm ? OBJ_obj2nid(algorithm->algorithm) : NID_undef;
        if (nid != NID_sha256 && nid != NID_sha384 && nid != NID_sha512 && nid != NID_sha3_256 &&
            nid != NID_sha3_384 && nid != NID_sha3_512)
        {
            row.integrity = SignatureIntegrity::Unsupported;
            row.detail = "ハッシュ方式が弱い、または未対応です。";
            return row;
        }
        row.digest = QString::fromLatin1(OBJ_nid2sn(nid));
        auto bytes = source.left(bounds.first) + source.mid(bounds.second, bounds.length);
        Owned<BIO, BIO_free> input(BIO_new_mem_buf(bytes.constData(), int(bytes.size())), BIO_free);
        Owned<BIO, BIO_free> output(BIO_new(BIO_s_null()), BIO_free);
        if (!input || !output)
            fail("署名検証用のメモリを確保できません。");
        cancelled(cancel);
        ERR_clear_error();
        if (CMS_verify(cms.get(), nullptr, nullptr, input.get(), output.get(),
                       CMS_BINARY | CMS_NO_SIGNER_CERT_VERIFY) != 1)
            fail("署名対象の改変、署名データの破損、または証明書不足を検出しました。");
        auto signer = sk_CMS_SignerInfo_value(signers, 0);
        X509* certificate = nullptr;
        CMS_SignerInfo_get0_algs(signer, nullptr, &certificate, nullptr, nullptr);
        if (!certificate)
            fail("署名者の証明書を取得できません。");
        row.certificate = certificate_detail::describeCertificate(certificate);
        const auto key = X509_get0_pubkey(certificate);
        if (!certificate_detail::supportedPublicKey(key))
        {
            row.integrity = SignatureIntegrity::Unsupported;
            row.detail = "公開鍵の方式または強度が今回の対応範囲外です。";
            return row;
        }
        row.integrity = SignatureIntegrity::Unchanged;
        auto certificates = CMS_get1_certs(cms.get());
        row.chain = checkChain(certificate, certificates, store);
        sk_X509_pop_free(certificates, X509_free);
        row.detail = row.entireFile ? "署名対象の改変は検出されませんでした。"
                                    : "署名した時点のバイト列は一致します。現在のファイルには署名対"
                                      "象外の追記があります。";
        if (row.certificate.extendedPurpose)
            row.detail += " 拡張用途が指定された証明書です。文書署名用途の評価は未実行です。";
    }
    catch (const std::exception& error)
    {
        cancelled(cancel);
        row.detail = QString::fromUtf8(error.what());
    }
    return row;
}
} // namespace
QString signatureIntegrityText(SignatureIntegrity value)
{
    switch (value)
    {
    case SignatureIntegrity::Unsigned:
        return "未署名の欄";
    case SignatureIntegrity::Unchanged:
        return "署名対象は一致";
    case SignatureIntegrity::Invalid:
        return "不一致／検証失敗";
    default:
        return "未対応／未評価";
    }
}
QString certificateChainText(CertificateChain value)
{
    switch (value)
    {
    case CertificateChain::LocalTrusted:
        return "ローカル信頼元まで確認";
    case CertificateChain::Untrusted:
        return "信頼元を確認できません";
    case CertificateChain::Expired:
        return "現在は期限切れ";
    case CertificateChain::NotYetValid:
        return "現在は有効期間前";
    case CertificateChain::Invalid:
        return "チェーン検証失敗";
    default:
        return "未評価";
    }
}
CertificateVerification verifyCertificateSignatures(const QString& path, const QByteArray& expected,
                                                    const CertificateTrust& trust,
                                                    const std::function<bool()>& cancel)
{
    cancelled(cancel);
    QFile input(path);
    if (expected.size() != 32 || !input.open(QIODevice::ReadOnly) || input.size() <= 0 ||
        input.size() > maxFile)
        fail("保存済みPDFと照合値が必要です。検証上限は256MiBです。");
    const auto source = input.read(maxFile + 1);
    if (source.size() != input.size() || source.size() > maxFile || input.error() != QFile::NoError)
        fail("署名付きPDFを読み取れません。");
    input.close();
    CertificateVerification result;
    result.fileHash = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
    result.checkedAt = QDateTime::currentDateTimeUtc();
    if (result.fileHash != expected)
        fail("PDFファイルが更新されました。開き直してから検証してください。");
    cancelled(cancel);
    PDFDocumentReader reader(
        nullptr,
        [](bool* ok)
        {
            *ok = false;
            return QString();
        },
        false, false);
    const auto doc = reader.readFromBuffer(source);
    if (reader.getReadingResult() != PDFDocumentReader::Result::OK ||
        doc.getStorage().getSecurityHandler()->getMode() != EncryptionMode::None)
        fail("このPDFの署名は検証できません。暗号化PDFの署名検証は未対応です。");
    PDFOpenSSLGlobalLock cryptoLock;
    auto store = roots(trust, cancel);
    std::set<PDFObjectReference> seen;
    QVector<PDFObject> values;
    int count = 0;
    std::function<void(PDFObject, QByteArray, QString, int)> visit;
    visit = [&](PDFObject object, QByteArray type, QString parent, int depth)
    {
        cancelled(cancel);
        if (++count > 100000 || depth > 32 ||
            (object.isReference() && !seen.insert(object.getReference()).second))
            fail("署名フォームが循環、共有、または上限を超えています。");
        object = doc.getObject(object);
        if (!object.isDictionary())
            fail("署名フォームの構造を読み取れません。");
        auto dictionary = object.getDictionary();
        auto ft = doc.getObject(dictionary->get("FT"));
        if (ft.isName())
            type = ft.getString();
        auto title = doc.getObject(dictionary->get("T"));
        if (title.isString())
        {
            PDFDocumentDataLoaderDecorator loader(&doc);
            const auto local = loader.readTextString(title, {});
            parent = parent.isEmpty() ? local : parent + "." + local;
        }
        const auto kids = doc.getObject(dictionary->get("Kids"));
        bool terminal = true;
        if (kids.isArray())
            for (const auto& kid : *kids.getArray())
            {
                const auto child = doc.getObject(kid);
                if (!child.isDictionary())
                    fail("署名フォームの子が不正です。");
                // Separate widgets inherit their parent's signature; don't duplicate the row.
                if (child.getDictionary()->hasKey("T") || child.getDictionary()->hasKey("FT") ||
                    child.getDictionary()->hasKey("Kids"))
                {
                    terminal = false;
                    visit(kid, type, parent, depth + 1);
                }
            }
        if (type == "Sig" && terminal)
        {
            if (result.signatures.size() >= 16)
                fail("証明書署名の検証上限は16件です。");
            const auto value = dictionary->get("V");
            values << doc.getObject(value);
            result.signatures << verifyOne(doc, value, parent, source, store.get(), cancel);
        }
    };
    const auto form = doc.getObject(doc.getCatalog()->getFormObject());
    if (form.isDictionary())
    {
        if (form.getDictionary()->hasKey("XFA"))
            fail("XFAの署名検証は未対応です。");
        const auto fields = doc.getObject(form.getDictionary()->get("Fields"));
        if (!fields.isNull() && !fields.isArray())
            fail("フォームFieldsが不正です。");
        if (fields.isArray())
            for (const auto& field : *fields.getArray())
                visit(field, {}, {}, 0);
    }
    for (const auto& entry : doc.getStorage().getObjects())
    {
        cancelled(cancel);
        const auto& object = entry.object;
        if (!object.isDictionary() || values.contains(object))
            continue;
        const auto& dictionary = *object.getDictionary();
        const auto type = doc.getObject(dictionary.get("Type"));
        if ((type.isName() && type.getString() == "Sig") || dictionary.hasKey("ByteRange"))
        {
            if (result.signatures.size() >= 16)
                fail("証明書署名の検証上限は16件です。");
            CertificateSignature row;
            row.field = "フォームに結び付かない署名辞書";
            row.detail = "署名辞書を検出しましたが、文書との結び付けが未対応です。";
            result.signatures << row;
        }
    }
    cancelled(cancel);
    if (fileHash(path) != expected)
        fail("検証中にPDFファイルが更新されました。結果は反映しません。");
    cancelled(cancel);
    ERR_clear_error();
    return result;
}
} // namespace tatsu
