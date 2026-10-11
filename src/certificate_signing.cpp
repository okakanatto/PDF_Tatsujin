#include "certificate_signing.h"
#include "certificate_crypto.h"
#include "pdf_objects.h"
#include "pdfcertificatestore.h"
#include "pdfdocumentbuilder.h"
#include "save_candidate.h"
#include "windows_path.h"
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>
#include <windows.h>

namespace tatsu
{
namespace
{
using namespace pdf;
using namespace detail;
template <typename T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
constexpr int reservedCms = 64 * 1024;
constexpr qint64 rangeMarker = 9000000000000000000LL;
void stop(const std::function<bool()>& cancel)
{
    if (cancel && cancel())
        fail("証明書署名の作成を中止しました。元の文書は保持しています。");
}
qint64 boundedIteration(const ASN1_INTEGER* value)
{
    int64_t number = 1;
    if (value && !ASN1_INTEGER_get_int64(&number, value))
        fail("P12の反復回数が不正です。");
    if (number < 1 || number > 1000000)
        fail("P12の反復回数が今回の対応範囲を超えています。");
    return number;
}
void checkCipher(const X509_ALGOR* algorithm)
{
    if (!algorithm || OBJ_obj2nid(algorithm->algorithm) != NID_pbes2 || !algorithm->parameter ||
        algorithm->parameter->type != V_ASN1_SEQUENCE)
        fail("P12の暗号方式はPBES2/AES/PBKDF2に対応しています。この方式は未対応です。");
    const auto bytes = algorithm->parameter->value.sequence;
    auto begin = ASN1_STRING_get0_data(bytes);
    const auto end = begin + ASN1_STRING_length(bytes);
    Owned<PBE2PARAM, PBE2PARAM_free> parameters(
        d2i_PBE2PARAM(nullptr, &begin, ASN1_STRING_length(bytes)), PBE2PARAM_free);
    if (!parameters || begin != end || !parameters->keyfunc || !parameters->encryption ||
        OBJ_obj2nid(parameters->keyfunc->algorithm) != NID_id_pbkdf2 ||
        !parameters->keyfunc->parameter || parameters->keyfunc->parameter->type != V_ASN1_SEQUENCE)
        fail("P12のPBES2パラメーターが不正または未対応です。");
    const auto cipher = OBJ_obj2nid(parameters->encryption->algorithm);
    if (cipher != NID_aes_128_cbc && cipher != NID_aes_192_cbc && cipher != NID_aes_256_cbc)
        fail("P12の暗号方式が未対応です。");
    const auto kdfBytes = parameters->keyfunc->parameter->value.sequence;
    begin = ASN1_STRING_get0_data(kdfBytes);
    const auto kdfEnd = begin + ASN1_STRING_length(kdfBytes);
    Owned<PBKDF2PARAM, PBKDF2PARAM_free> kdf(
        d2i_PBKDF2PARAM(nullptr, &begin, ASN1_STRING_length(kdfBytes)), PBKDF2PARAM_free);
    if (!kdf || begin != kdfEnd || !kdf->iter || !kdf->salt ||
        kdf->salt->type != V_ASN1_OCTET_STRING ||
        ASN1_STRING_length(kdf->salt->value.octet_string) > 64)
        fail("P12のPBKDF2パラメーターが不正です。");
    boundedIteration(kdf->iter);
    if (kdf->keylength)
    {
        int64_t length = 0;
        if (!ASN1_INTEGER_get_int64(&length, kdf->keylength) || length < 16 || length > 64)
            fail("P12の鍵長が不正です。");
    }
    if (kdf->prf)
    {
        const auto prf = OBJ_obj2nid(kdf->prf->algorithm);
        if (prf != NID_hmacWithSHA1 && prf != NID_hmacWithSHA256 && prf != NID_hmacWithSHA384 &&
            prf != NID_hmacWithSHA512)
            fail("P12の鍵導出方式が未対応です。");
    }
}
struct KeyMaterial
{
    Owned<EVP_PKEY, EVP_PKEY_free> key{nullptr, EVP_PKEY_free};
    Owned<X509, X509_free> certificate{nullptr, X509_free};
    struct ChainFree
    {
        void operator()(STACK_OF(X509) * value) const
        {
            sk_X509_pop_free(value, X509_free);
        }
    };
    std::unique_ptr<STACK_OF(X509), ChainFree> chain;
};
KeyMaterial loadKey(const QByteArray& bytes, const QString& password,
                    const std::function<bool()>& cancel)
{
    stop(cancel);
    if (bytes.isEmpty() || bytes.size() > 4 * 1024 * 1024 || password.size() > 1024 ||
        password.contains(QChar(0)) || QString::fromUtf8(password.toUtf8()) != password)
        fail("P12の大きさまたはパスワードが不正です。P12上限は4MiBです。");
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.constData());
    Owned<PKCS12, PKCS12_free> p12(d2i_PKCS12(nullptr, &cursor, bytes.size()), PKCS12_free);
    if (!p12 ||
        cursor != reinterpret_cast<const unsigned char*>(bytes.constData() + bytes.size()) ||
        !PKCS12_mac_present(p12.get()))
        fail("MAC付きのPKCS#12ファイルを読み取れません。");
    const ASN1_INTEGER* iteration = nullptr;
    PKCS12_get0_mac(nullptr, nullptr, nullptr, &iteration, p12.get());
    boundedIteration(iteration);
    auto pass = password.toUtf8();
    struct Erase
    {
        QByteArray& data;
        ~Erase()
        {
            data.detach();
            OPENSSL_cleanse(data.data(), data.size());
        }
    } erase{pass};
    if (PKCS12_verify_mac(p12.get(), pass.constData(), pass.size()) != 1)
        fail("P12のパスワードまたはファイルを確認できません。");
    struct SafesFree
    {
        void operator()(STACK_OF(PKCS7) * value) const
        {
            sk_PKCS7_pop_free(value, PKCS7_free);
        }
    };
    struct BagsFree
    {
        void operator()(STACK_OF(PKCS12_SAFEBAG) * value) const
        {
            sk_PKCS12_SAFEBAG_pop_free(value, PKCS12_SAFEBAG_free);
        }
    };
    std::unique_ptr<STACK_OF(PKCS7), SafesFree> safes(PKCS12_unpack_authsafes(p12.get()));
    if (!safes || sk_PKCS7_num(safes.get()) > 32)
        fail("P12の格納構造が不正または上限を超えています。");
    int keys = 0, certificates = 0, total = 0;
    std::function<void(const STACK_OF(PKCS12_SAFEBAG)*, int)> visit;
    visit = [&](const STACK_OF(PKCS12_SAFEBAG) * bags, int depth)
    {
        if (!bags || depth > 8)
            fail("P12の格納構造が不正です。");
        for (int i = 0; i < sk_PKCS12_SAFEBAG_num(bags); ++i)
        {
            stop(cancel);
            if (++total > 1000)
                fail("P12の格納要素が上限を超えています。");
            const auto bag = sk_PKCS12_SAFEBAG_value(bags, i);
            switch (PKCS12_SAFEBAG_get_nid(bag))
            {
            case NID_keyBag:
                ++keys;
                break;
            case NID_pkcs8ShroudedKeyBag:
            {
                ++keys;
                const X509_ALGOR* algorithm = nullptr;
                const auto encrypted = PKCS12_SAFEBAG_get0_pkcs8(bag);
                if (!encrypted)
                    fail("P12の秘密鍵形式が不正です。");
                X509_SIG_get0(encrypted, &algorithm, nullptr);
                checkCipher(algorithm);
                break;
            }
            case NID_certBag:
                ++certificates;
                break;
            case NID_safeContentsBag:
                visit(PKCS12_SAFEBAG_get0_safes(bag), depth + 1);
                break;
            default:
                fail("P12に未対応の格納要素があります。");
            }
        }
    };
    for (int i = 0; i < sk_PKCS7_num(safes.get()); ++i)
    {
        stop(cancel);
        auto safe = sk_PKCS7_value(safes.get(), i);
        STACK_OF(PKCS12_SAFEBAG)* unpacked = nullptr;
        if (PKCS7_type_is_data(safe))
            unpacked = PKCS12_unpack_p7data(safe);
        else if (PKCS7_type_is_encrypted(safe))
        {
            if (!safe->d.encrypted || !safe->d.encrypted->enc_data)
                fail("P12の暗号化構造が不正です。");
            checkCipher(safe->d.encrypted->enc_data->algorithm);
            unpacked = PKCS12_unpack_p7encdata(safe, pass.constData(), pass.size());
        }
        else
            fail("P12の格納方式が未対応です。");
        std::unique_ptr<STACK_OF(PKCS12_SAFEBAG), BagsFree> bags(unpacked);
        visit(bags.get(), 0);
    }
    if (keys != 1 || certificates < 1 || certificates > 17)
        fail("P12は秘密鍵1個と証明書／チェーン16件までに対応しています。");
    stop(cancel);
    EVP_PKEY* key = nullptr;
    X509* certificate = nullptr;
    STACK_OF(X509)* chain = nullptr;
    const auto parsed = PKCS12_parse(p12.get(), pass.constData(), &key, &certificate, &chain);
    KeyMaterial material;
    material.key.reset(key);
    material.certificate.reset(certificate);
    material.chain.reset(chain);
    if (parsed != 1 || !key || !certificate || X509_check_private_key(certificate, key) != 1)
        fail("P12の秘密鍵と証明書を確認できません。");
    if (!certificate_detail::supportedPublicKey(key))
        fail("RSA 2048bit以上またはEC 256bit以上の証明書が必要です。");
    const auto before = X509_cmp_current_time(X509_get0_notBefore(certificate));
    const auto after = X509_cmp_current_time(X509_get0_notAfter(certificate));
    if (before >= 0 || after <= 0)
        fail("証明書の有効期間を確認してください。期限切れ・期間前・不正な期間では署名しません。");
    if (X509_get_ext_by_NID(certificate, NID_key_usage, -1) >= 0)
    {
        Owned<ASN1_BIT_STRING, ASN1_BIT_STRING_free> usage(
            static_cast<ASN1_BIT_STRING*>(
                X509_get_ext_d2i(certificate, NID_key_usage, nullptr, nullptr)),
            ASN1_BIT_STRING_free);
        if (!usage ||
            !(ASN1_BIT_STRING_get_bit(usage.get(), 0) || ASN1_BIT_STRING_get_bit(usage.get(), 1)))
            fail("この証明書のKeyUsageでは署名できません。");
    }
    stop(cancel);
    return material;
}
std::vector<PDFObject> array(const PDFDocument& document, const PDFObject& object)
{
    const auto resolved = document.getObject(object);
    if (resolved.isNull())
        return {};
    if (!resolved.isArray())
        fail("フォームまたはページ注釈の配列が不正です。");
    return {resolved.getArray()->begin(), resolved.getArray()->end()};
}
QByteArray cmsSignature(KeyMaterial& material, const QByteArray& data)
{
    Owned<BIO, BIO_free> input(BIO_new_mem_buf(data.constData(), int(data.size())), BIO_free);
    Owned<CMS_ContentInfo, CMS_ContentInfo_free> cms(
        CMS_sign(nullptr, nullptr, material.chain.get(), nullptr,
                 CMS_PARTIAL | CMS_BINARY | CMS_DETACHED),
        CMS_ContentInfo_free);
    if (!input || !cms ||
        !CMS_add1_signer(cms.get(), material.certificate.get(), material.key.get(), EVP_sha256(),
                         CMS_NOSMIMECAP) ||
        CMS_final(cms.get(), input.get(), nullptr, CMS_BINARY | CMS_DETACHED) != 1)
        fail("CMS署名を生成できません。コピーは確定していません。");
    const auto length = i2d_CMS_ContentInfo(cms.get(), nullptr);
    if (length <= 0 || length > reservedCms)
        fail("CMS署名が予約領域64KiBを超えています。");
    QByteArray der(length, Qt::Uninitialized);
    auto cursor = reinterpret_cast<unsigned char*>(der.data());
    if (i2d_CMS_ContentInfo(cms.get(), &cursor) != length)
        fail("CMS署名を符号化できません。");
    return der;
}
} // namespace
CertificateIdentity inspectSigningCertificate(const QByteArray& p12, const QString& password)
{
    PDFOpenSSLGlobalLock lock;
    auto material = loadKey(p12, password, {});
    return certificate_detail::describeCertificate(material.certificate.get());
}
SignedPdfCandidate prepareSignedPdf(const PDFDocument& document, const QByteArray& p12,
                                    const QString& password, const QString& reason,
                                    const std::function<bool()>& cancel)
{
    stop(cancel);
    if (!document.getCatalog() || !document.getCatalog()->getPageCount())
        fail("署名するPDFを開いてください。");
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction + "。新しい署名は作成しません。");
    if (reason.size() > 1000 || QString::fromUtf8(reason.toUtf8()) != reason)
        fail("署名理由は1000文字までの有効なUnicodeで指定してください。");
    PDFOpenSSLGlobalLock lock;
    auto material = loadKey(p12, password, cancel);
    SignedPdfCandidate result;
    result.certificate = certificate_detail::describeCertificate(material.certificate.get());
    result.field = "TatsujinCertificate_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto storage = document.getStorage();
    QByteArray placeholder(reservedCms, '\0');
    for (int i = 0; i < 32; i += 4)
    {
        const auto nonce = QRandomGenerator::system()->generate();
        memcpy(placeholder.data() + i, &nonce, sizeof(nonce));
    }
    // The fixed SDK writes strings as hex when they contain a delimiter.
    // Keep a unique nonce and force a stable hex reservation on every run.
    placeholder[31] = '(';
    PDFDictionary signature;
    set(signature, "Type", PDFObject::createName("Sig"));
    set(signature, "Filter", PDFObject::createName("Adobe.PPKLite"));
    set(signature, "SubFilter", PDFObject::createName("adbe.pkcs7.detached"));
    set(signature, "ByteRange",
        arrObject(std::vector<PDFObject>(4, PDFObject::createInteger(rangeMarker))));
    set(signature, "Contents", PDFObject::createString(placeholder));
    set(signature, "M",
        PDFObject::createString(
            QDateTime::currentDateTimeUtc().toString("'D:'yyyyMMddHHmmss'Z'").toLatin1()));
    if (!reason.isEmpty())
        set(signature, "Reason", PDFObjectFactory::createTextString(reason));
    const auto value = storage.addObject(dictObject(signature));
    PDFDictionary field;
    set(field, "FT", PDFObject::createName("Sig"));
    set(field, "T", PDFObject::createString(result.field.toLatin1()));
    set(field, "V", PDFObject::createReference(value));
    set(field, "Ff", PDFObject::createInteger(1));
    const auto fieldReference = storage.addObject(dictObject(field));
    const auto pageReference = document.getCatalog()->getPage(0)->getPageReference();
    PDFDictionary widget;
    set(widget, "Type", PDFObject::createName("Annot"));
    set(widget, "Subtype", PDFObject::createName("Widget"));
    set(widget, "Parent", PDFObject::createReference(fieldReference));
    set(widget, "P", PDFObject::createReference(pageReference));
    set(widget, "Rect", rectObject({0, 0, 0, 0}));
    set(widget, "F", PDFObject::createInteger(3));
    const auto widgetReference = storage.addObject(dictObject(widget));
    set(field, "Kids", arrObject({PDFObject::createReference(widgetReference)}));
    storage.setObject(fieldReference, dictObject(field));
    auto page = *storage.getObjectByReference(pageReference).getDictionary();
    auto annotations = array(document, page.get("Annots"));
    annotations.push_back(PDFObject::createReference(widgetReference));
    set(page, "Annots", arrObject(std::move(annotations)));
    storage.setObject(pageReference, dictObject(page));
    const auto root = document.getTrailerDictionary()->get("Root");
    if (!root.isReference())
        fail("PDFカタログ参照が不正です。");
    auto catalog = *storage.getObject(root).getDictionary();
    const auto formObject = document.getObject(catalog.get("AcroForm"));
    if (!formObject.isNull() && !formObject.isDictionary())
        fail("AcroFormが不正です。");
    auto form = formObject.isDictionary() ? *formObject.getDictionary() : PDFDictionary{};
    auto fields = array(document, form.get("Fields"));
    fields.push_back(PDFObject::createReference(fieldReference));
    set(form, "Fields", arrObject(std::move(fields)));
    const auto flags = document.getObject(form.get("SigFlags"));
    if (!flags.isNull() && (!flags.isInt() || flags.getInteger() < 0))
        fail("SigFlagsが不正です。");
    set(form, "SigFlags", PDFObject::createInteger((flags.isInt() ? flags.getInteger() : 0) | 3));
    const auto formReference = storage.addObject(dictObject(form));
    set(catalog, "AcroForm", PDFObject::createReference(formReference));
    storage.setObject(root.getReference(), dictObject(catalog));
    stop(cancel);
    const PDFDocument candidate(std::move(storage), document.getInfo()->version,
                                document.getSourceDataHash());
    auto bytes = encodePdf(candidate);
    if (bytes.size() > 256 * 1024 * 1024)
        fail("署名したコピーの上限は256MiBです。");
    const auto contents = '<' + placeholder.toHex() + '>';
    const auto first = bytes.indexOf(contents);
    if (first <= 0 || bytes.indexOf(contents, first + 1) >= 0)
        fail("署名予約領域を一意に確認できません。");
    const qint64 second = first + contents.size();
    const qint64 ranges[4] = {0, first, second, bytes.size() - second};
    const auto marker = QByteArray::number(rangeMarker);
    auto position = first;
    for (int i = 3; i >= 0; --i)
    {
        position = bytes.lastIndexOf(marker, position - 1);
        if (position < 0)
            fail("ByteRangeの予約領域を確認できません。");
        const auto replacement = QByteArray::number(ranges[i]).leftJustified(marker.size(), ' ');
        bytes.replace(position, marker.size(), replacement);
    }
    stop(cancel);
    const auto der = cmsSignature(material, bytes.left(first) + bytes.mid(second));
    bytes.replace(first + 1, reservedCms * 2, der.toHex().leftJustified(reservedCms * 2, '0'));
    result.bytes = std::move(bytes);
    stop(cancel);
    return result;
}
QByteArray exportSignedPdf(const PDFDocument& document, const QByteArray& p12,
                           const QString& password, const QString& reason,
                           const QString& destination, const std::function<bool()>& cancel,
                           const std::function<void(QString)>& progress,
                           const std::function<void()>& validateInputs)
{
    stop(cancel);
    if (validateInputs)
        validateInputs();
    if (destination.isEmpty() || !destination.endsWith(".pdf", Qt::CaseInsensitive) ||
        QFileInfo::exists(destination) || !QFileInfo(destination).dir().exists())
        fail("存在するフォルダに、まだ使われていない.pdfの保存先を選んでください。");
    if (progress)
        progress("証明書と秘密鍵を確認し、現在の文書のコピーへ署名しています…");
    const auto candidate = prepareSignedPdf(document, p12, password, reason, cancel);
    SaveCandidate staged(QFileInfo(destination).absolutePath());
    QFile file(staged.filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(candidate.bytes) != candidate.bytes.size() || !file.flush())
        fail("署名した保存候補を書き込めません。");
    file.close();
    stop(cancel);
    if (progress)
        progress("署名対象・証明書指紋・PDFの保持を検証しています…");
    const auto hash = fileHash(staged.filePath());
    const auto verified = verifyCertificateSignatures(staged.filePath(), hash, {false, {}}, cancel);
    const auto found =
        std::find_if(verified.signatures.begin(), verified.signatures.end(),
                     [&](const CertificateSignature& row) { return row.field == candidate.field; });
    if (found == verified.signatures.end() || found->integrity != SignatureIntegrity::Unchanged ||
        !found->entireFile || found->certificate.fingerprint != candidate.certificate.fingerprint)
        fail("作成した証明書署名を正しく確認できません。保存先は保持しています。");
    const auto reopened = readPdf(staged.filePath());
    if (reopened.getCatalog()->getPageCount() != document.getCatalog()->getPageCount() ||
        editingRestriction(reopened).isEmpty())
        fail("署名付きコピーのページと編集制限を確認できません。");
    stop(cancel);
    const auto from = extendedWindowsPath(staged.filePath());
    const auto to = extendedWindowsPath(QFileInfo(destination).absoluteFilePath());
    if (progress)
        progress("署名したコピーを新しいPDFへ保存しています…");
    stop(cancel);
    if (validateInputs)
        validateInputs();
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString(
                 "署名したコピーを確定できません（Windows %1）。既存ファイルは変更していません。")
                 .arg(GetLastError()));
    return hash;
}
} // namespace tatsu
