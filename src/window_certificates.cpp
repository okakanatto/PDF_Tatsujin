#include "certificate_dialog.h"
#include "certificate_signing_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::exportSignedCertificateCopy()
{
    doc.editable();
    canvas->finishFormEdit();
    const auto base = doc.target.isEmpty() ? doc.source : doc.target;
    const auto info = QFileInfo(base);
    const auto suggested =
        base.isEmpty() ? QString()
                       : info.dir().absoluteFilePath(info.completeBaseName() + "_証明書署名.pdf");
    QVector<QPair<QString, QByteArray>> originals;
    if (!doc.source.isEmpty())
        originals << qMakePair(doc.source, doc.sourceHash);
    if (!doc.target.isEmpty() && !sameFilePath(doc.source, doc.target))
        originals << qMakePair(doc.target, doc.targetHash);
    CertificateSigningDialog dialog(doc.pdf(), originals, suggested, this);
    if (dialog.exec() != QDialog::Accepted || dialog.savedPath().isEmpty())
        return;
    if (fileHash(dialog.savedPath()) != dialog.savedHash())
        fail("保存した署名付きコピーが更新されました。元の文書は保持しています。");
    auto window = std::make_unique<Window>();
    window->doc.open(dialog.savedPath());
    if (window->doc.sourceHash != dialog.savedHash() || window->doc.readOnly.isEmpty())
        fail("署名したコピーを確認できません。元の文書は保持しています。");
    window->setObjectName("signedCertificateCreatedDocument");
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->refresh(true);
    window->show();
    window->canvas->goToPage(0);
    window.release();
    status->setText(
        "署名したコピーを読み取り専用で開きました。作成メニューから証明書署名を確認できます。");
}
void Window::verifyDocumentCertificates()
{
    if (!doc.loaded() || doc.busy)
        fail("保存済みPDFを開いてください。");
    canvas->finishFormEdit();
    if (doc.dirty())
        fail("未保存の変更があります。保存して開き直してから証明書署名を確認してください。");
    const auto path = doc.target.isEmpty() ? doc.source : doc.target;
    const auto hash = doc.target.isEmpty() ? doc.sourceHash : doc.targetHash;
    CertificateDialog dialog(path, hash, this);
    dialog.exec();
}
} // namespace tatsu
