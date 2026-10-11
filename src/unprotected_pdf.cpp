#include "unprotected_pdf.h"
#include "pdf_objects.h"
#include "pdfoptimizer.h"
#include "pdfsecurityhandler.h"
#include "save_candidate.h"
#include "windows_path.h"
#include <windows.h>

namespace tatsu
{
namespace
{
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("編集用コピーの作成を中止しました。元の文書は変更していません。");
}
} // namespace
PDFDocument unprotectPdf(const PDFDocument& document, const QString& ownerPassword,
                         const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (!document.getCatalog() || !document.getCatalog()->getPageCount() ||
        !document.getStorage().getSecurityHandler())
        fail("パスワードで保護されたPDFを開いてください。");
    const auto security = document.getStorage().getSecurityHandler();
    if (security->getMode() != EncryptionMode::Standard)
        fail("標準のパスワード保護されたPDFだけに対応しています。");
    if (ownerPassword.size() > 1024 || QString::fromUtf8(ownerPassword.toUtf8()) != ownerPassword)
        fail("パスワードの長さまたはUnicode文字が不正です。");
    std::unique_ptr<PDFSecurityHandler> authentication(security->clone());
    int attempts = 0;
    bool supplied = false;
    const auto authorization = authentication->authenticate(
        [&](bool* ok)
        {
            *ok = attempts++ == 0;
            supplied |= *ok;
            return ownerPassword;
        },
        true);
    if (authorization != PDFSecurityHandler::AuthorizationResult::OwnerAuthorized ||
        (!supplied && !ownerPassword.isEmpty()))
        fail("変更権限パスワードを確認できません。閲覧用パスワードでは解除できません。");
    stop(cancelled);
    auto storage = document.getStorage();
    auto trailer = *document.getTrailerDictionary();
    trailer.removeEntry("Encrypt");
    storage.setTrailerDictionary(detail::dictObject(trailer));
    storage.setSecurityHandler(PDFSecurityHandler::createSecurityHandler({}, {}));
    PDFDocument candidate(PDFObjectStorage(storage), document.getInfo()->version,
                          document.getSourceDataHash());
    const auto restriction = editingRestriction(candidate);
    if (!restriction.isEmpty())
        fail(restriction + "。編集用コピーは作成しません。");
    for (const auto& entry : storage.getObjects())
    {
        stop(cancelled);
        if (!entry.object.isStream())
            continue;
        const auto filter =
            storage.getObject(entry.object.getStream()->getDictionary()->get("Filter"));
        auto crypt = [&](const PDFObject& value)
        {
            const auto resolved = storage.getObject(value);
            return resolved.isName() && resolved.getString() == "Crypt";
        };
        if (crypt(filter) || (filter.isArray() && std::any_of(filter.getArray()->begin(),
                                                              filter.getArray()->end(), crypt)))
            fail("個別のCryptフィルターがあるPDFには対応していません。保護は解除しません。");
    }
    // Drop the now unreachable encryption dictionary without renumbering any
    // reachable page, form, annotation or appearance reference.
    PDFOptimizer optimizer(PDFOptimizer::RemoveUnusedObjects, nullptr);
    optimizer.setStorage(storage);
    QObject::connect(
        &optimizer, &PDFOptimizer::optimizationProgress, &optimizer,
        [&](const QString&) { stop(cancelled); }, Qt::DirectConnection);
    optimizer.optimize();
    stop(cancelled);
    return PDFDocument(optimizer.takeStorage(), document.getInfo()->version,
                       document.getSourceDataHash());
}
QByteArray exportUnprotectedPdf(const PDFDocument& document, const QString& ownerPassword,
                                const QString& destination, const std::function<bool()>& cancelled,
                                const std::function<void(QString)>& progress)
{
    stop(cancelled);
    if (destination.isEmpty() || !destination.endsWith(".pdf", Qt::CaseInsensitive) ||
        QFileInfo::exists(destination) || !QFileInfo(destination).dir().exists())
        fail("存在するフォルダに、まだ使われていない.pdfの保存先を選んでください。");
    if (progress)
        progress("変更権限を確認し、編集用コピーを準備しています…");
    auto candidate = unprotectPdf(document, ownerPassword, cancelled);
    stop(cancelled);
    SaveCandidate staged(QFileInfo(destination).absolutePath());
    if (progress)
        progress("パスワードなしのPDFを検証しています…");
    writeCandidate(candidate, staged.filePath());
    stop(cancelled);
    const auto check = readPdf(staged.filePath());
    if (check.getTrailerDictionary()->hasKey("Encrypt") ||
        check.getStorage().getSecurityHandler()->getMode() != EncryptionMode::None ||
        !editingRestriction(check).isEmpty() ||
        check.getCatalog()->getPageCount() != document.getCatalog()->getPageCount())
        fail("編集用コピーを正しく確認できません。保存先は変更していません。");
    const auto hash = fileHash(staged.filePath());
    if (progress)
        progress("編集用コピーを保存しています…");
    stop(cancelled);
    const auto from = extendedWindowsPath(staged.filePath()),
               to = extendedWindowsPath(QFileInfo(destination).absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString("コピーの確定に失敗しました（Windows %1）。既存ファイルは変更していません。")
                 .arg(GetLastError()));
    return hash;
}
} // namespace tatsu
