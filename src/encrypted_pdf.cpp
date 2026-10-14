#include "encrypted_pdf.h"
#include "pdf_objects.h"
#include "pdf_security_adapter.h"
#include "pdfsecurityhandler.h"
#include "save_candidate.h"
#include "windows_path.h"
#include <QXmlStreamReader>
#include <windows.h>

namespace tatsu
{
using namespace detail;
namespace
{
struct Range
{
    uint first, last;
};
bool in(uint value, std::initializer_list<Range> ranges)
{
    for (auto range : ranges)
        if (value >= range.first && value <= range.last)
            return true;
    return false;
}
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("保護したコピーの作成を中止しました。文書は変更していません。");
}
void rejectPdfA(const PDFDocument& document)
{
    const auto catalog = document.getObject(document.getTrailerDictionary()->get("Root"));
    const auto metadata = document.getObject(catalog.getDictionary()->get("Metadata"));
    if (metadata.isNull())
        return;
    if (!metadata.isStream())
        fail("メタデータを確認できません。保護したコピーは作成しません。");
    const auto xml = document.getDecodedStream(metadata.getStream());
    if (xml.size() > 1024 * 1024)
        fail("メタデータが大きすぎるため、保護したコピーは作成しません。");
    QXmlStreamReader reader(xml);
    while (!reader.atEnd())
    {
        reader.readNext();
        if (reader.namespaceUri() == "http://www.aiim.org/pdfa/ns/id/")
            fail("PDF/Aは暗号化できません。通常PDFへの変換が必要です。");
        for (const auto& attribute : reader.attributes())
            if (attribute.namespaceUri() == "http://www.aiim.org/pdfa/ns/id/")
                fail("PDF/Aは暗号化できません。通常PDFへの変換が必要です。");
        if (reader.isDTD())
            fail("DTDを含むメタデータには対応していません。");
    }
    if (reader.hasError())
        fail("メタデータの形式を確認できません。保護したコピーは作成しません。");
}
} // namespace
QString preparePdfPassword(const QString& password)
{
    // RFC 4013 mapping and Unicode 3.2 normalization; independently checked
    // against Python's stringprep tables. No silent UTF-8 truncation.
    if (password.toUtf8().size() > 1024 || QString::fromUtf8(password.toUtf8()) != password)
        fail("パスワードの長さまたはUnicode文字が不正です。");
    QString mapped;
    for (auto value : password.toUcs4())
    {
        if (in(value, {{0xad, 0xad},
                       {0x34f, 0x34f},
                       {0x1806, 0x1806},
                       {0x180b, 0x180d},
                       {0x200b, 0x200d},
                       {0x2060, 0x2060},
                       {0xfe00, 0xfe0f},
                       {0xfeff, 0xfeff}}))
            continue;
        if (in(value, {{0xa0, 0xa0},
                       {0x1680, 0x1680},
                       {0x2000, 0x200b},
                       {0x202f, 0x202f},
                       {0x205f, 0x205f},
                       {0x3000, 0x3000}}))
            mapped += QChar(' ');
        else
        {
            const char32_t c = value;
            mapped += QString::fromUcs4(&c, 1);
        }
    }
    const auto prepared = mapped.normalized(QString::NormalizationForm_KC, QChar::Unicode_3_2);
    const auto values = prepared.toUcs4();
    if (prepared.isEmpty() || prepared.toUtf8().size() > 127)
        fail("パスワードは標準PDFの変換後に1〜127 UTF-8 "
             "bytesで指定してください。切り詰めは行いません。");
    bool right = false, left = false;
    for (auto value : values)
    {
        const auto version = QChar::unicodeVersion(value);
        if (version == QChar::Unicode_Unassigned || version > QChar::Unicode_3_2 ||
            in(value,
               {{0, 0x1f},           {0x7f, 0x9f},       {0x6dd, 0x6dd},     {0x70f, 0x70f},
                {0x180e, 0x180e},    {0x200c, 0x200f},   {0x2028, 0x202e},   {0x2060, 0x2063},
                {0x206a, 0x206f},    {0x2ff0, 0x2ffb},   {0x340, 0x341},     {0xd800, 0xdfff},
                {0xe000, 0xf8ff},    {0xfdd0, 0xfdef},   {0xfeff, 0xfeff},   {0xfff9, 0xfffd},
                {0x1d173, 0x1d17a},  {0xe0001, 0xe0001}, {0xe0020, 0xe007f}, {0xf0000, 0xffffd},
                {0x100000, 0x10fffd}}) ||
            (value & 0xffff) >= 0xfffe)
            fail("パスワードにPDFの準備規則で禁止・未対応の文字が含まれています。");
        const auto direction = QChar::direction(value);
        right |= direction == QChar::DirR || direction == QChar::DirAL;
        left |= direction == QChar::DirL;
    }
    auto rtl = [](uint value)
    {
        const auto direction = QChar::direction(value);
        return direction == QChar::DirR || direction == QChar::DirAL;
    };
    if (right && (left || !rtl(values.first()) || !rtl(values.last())))
        fail("パスワードの右から左へ書く文字の組合せが不正です。");
    if (sdkPreparedPassword(password) != prepared.toUtf8())
        fail("このパスワードのUnicode互換変換には対応できません。別のパスワードを指定してください"
             "。");
    return prepared;
}
PDFDocument protectPdf(const PDFDocument& document, const EncryptionOptions& options)
{
    if (!document.getCatalog() || !document.getCatalog()->getPageCount() ||
        !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    rejectPdfA(document);
    PDFSecurityHandlerFactory::SecuritySettings settings;
    settings.algorithm = PDFSecurityHandlerFactory::AES_256;
    settings.encryptContents = PDFSecurityHandlerFactory::All;
    settings.userPassword = preparePdfPassword(options.userPassword);
    settings.ownerPassword = preparePdfPassword(options.ownerPassword);
    if (settings.userPassword == settings.ownerPassword)
        fail("閲覧用と変更権限用には異なるパスワードを指定してください。");
    settings.id = document.getIdPart(0);
    if (settings.id.isEmpty())
        settings.id =
            PDFSecurityHandlerFactory::generateRandomByteArray(*QRandomGenerator::system(), 16);
    settings.permissions = 0xc0 | uint32_t(PDFSecurityHandler::Permission::Accessibility);
    auto allow = [&](bool value, PDFSecurityHandler::Permission permission)
    {
        if (value)
            settings.permissions |= uint32_t(permission);
    };
    allow(options.print, PDFSecurityHandler::Permission::PrintLowResolution);
    allow(options.print, PDFSecurityHandler::Permission::PrintHighResolution);
    allow(options.copy, PDFSecurityHandler::Permission::CopyContent);
    allow(options.forms, PDFSecurityHandler::Permission::ModifyFormFields);
    allow(options.annotations, PDFSecurityHandler::Permission::ModifyInteractiveItems);
    allow(options.assemble, PDFSecurityHandler::Permission::Assemble);
    allow(options.modify, PDFSecurityHandler::Permission::Modify);
    auto handler = PDFSecurityHandlerFactory::createSecurityHandler(settings);
    if (!handler)
        fail("PDFの暗号化を準備できません。");
    const auto encryptionObject = handler->createEncryptionDictionaryObject();
    const auto encryption = encryptionObject.getDictionary();
    const PDFDocumentDataLoaderDecorator loader(&document);
    if (loader.readInteger(encryption->get("R"), 0) != 6 ||
        loader.readInteger(encryption->get("V"), 0) != 5)
        fail("AES-256 R6の条件を確認できません。");
    auto storage = document.getStorage();
    auto trailer = *document.getTrailerDictionary();
    if (document.getIdPart(0).isEmpty())
    {
        set(trailer, "ID",
            arrObject(
                {PDFObject::createString(settings.id), PDFObject::createString(settings.id)}));
    }
    const auto root = trailer.get("Root");
    if (!root.isReference())
        fail("PDFの文書参照を確認できません。");
    auto catalog = *storage.getObject(root).getDictionary();
    set(catalog, "Version", PDFObject::createName("2.0"));
    storage.setObject(root.getReference(), dictObject(catalog));
    const auto reference = storage.addObject(encryptionObject);
    set(trailer, "Encrypt", PDFObject::createReference(reference));
    set(trailer, "Size", PDFObject::createInteger(storage.getObjects().size()));
    storage.setTrailerDictionary(dictObject(trailer));
    storage.setSecurityHandler(std::move(handler));
    // The SDK builder updates Producer/ModDate. A copy export keeps both exact.
    return PDFDocument(std::move(storage), PDFVersion(2, 0), document.getSourceDataHash());
}
void exportProtectedPdf(const PDFDocument& document, const EncryptionOptions& options,
                        const QString& destination, const std::function<bool()>& cancelled,
                        const std::function<void(QString)>& progress)
{
    stop(cancelled);
    if (destination.isEmpty() || !destination.endsWith(".pdf", Qt::CaseInsensitive) ||
        QFileInfo::exists(destination) || !QFileInfo(destination).dir().exists())
        fail("存在するフォルダに、まだ使われていない.pdfの保存先を選んでください。");
    if (progress)
        progress("パスワードで保護したPDFを作成しています…");
    auto candidate = protectPdf(document, options);
    stop(cancelled);
    SaveCandidate staged(QFileInfo(destination).absolutePath());
    QFile file(staged.filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        fail("保存候補を作成できません。");
    const auto data = encodePdf(candidate);
    if (file.write(data) != data.size() || !file.flush())
        fail("暗号化したPDFの書込みに失敗しました。");
    file.close();
    stop(cancelled);
    if (progress)
        progress("暗号化とページ・パスワードを検証しています…");
    int role = 0;
    for (const auto& password : {options.userPassword, options.ownerPassword})
    {
        auto check = readPdf(staged.filePath(), password);
        const auto security = check.getStorage().getSecurityHandler();
        const auto expected = role++ == 0
                                  ? PDFSecurityHandler::AuthorizationResult::UserAuthorized
                                  : PDFSecurityHandler::AuthorizationResult::OwnerAuthorized;
        if (check.getCatalog()->getPageCount() != document.getCatalog()->getPageCount() ||
            security->getMode() != EncryptionMode::Standard || !security->isMetadataEncrypted() ||
            security->getAuthorizationResult() != expected)
            fail("暗号化したPDFを正しく開けません。保存先は変更していません。");
    }
    if (progress)
        progress("コピーを保存しています…");
    stop(cancelled);
    const auto from = extendedWindowsPath(staged.filePath()),
               to = extendedWindowsPath(QFileInfo(destination).absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString("コピーの確定に失敗しました（Windows %1）。既存ファイルは変更していません。")
                 .arg(GetLastError()));
}
} // namespace tatsu
