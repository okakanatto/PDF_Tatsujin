#include "decryption_dialog.h"
#include "encryption_dialog.h"
#include "pdfsecurityhandler.h"
#include "window.h"

namespace tatsu
{
void Window::exportEncryptedCopy()
{
    doc.editable();
    canvas->finishFormEdit();
    const auto base = doc.target.isEmpty() ? doc.source : doc.target;
    const auto info = QFileInfo(base);
    const auto suggested =
        base.isEmpty() ? QString()
                       : info.dir().absoluteFilePath(info.completeBaseName() + "-protected.pdf");
    EncryptionDialog dialog(doc.pdf(), suggested, this);
    if (dialog.exec() == QDialog::Accepted)
        status->setText("保護したコピーを保存しました：" +
                        QFileInfo(dialog.savedPath()).fileName());
}
void Window::createEditableCopy()
{
    if (!doc.loaded() || doc.busy ||
        doc.pdf().getStorage().getSecurityHandler()->getMode() != EncryptionMode::Standard)
        fail("パスワードで保護されたPDFを開いてください。");
    const auto info = QFileInfo(doc.source);
    DecryptionDialog dialog(
        doc.pdf(), info.dir().absoluteFilePath(info.completeBaseName() + "-editable.pdf"), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (fileHash(dialog.savedPath()) != dialog.savedHash())
        fail("保存したコピーが更新されたため、自動で開きません。保存先を確認してください。");
    auto window = std::make_unique<Window>();
    window->doc.open(dialog.savedPath());
    if (window->doc.sourceHash != dialog.savedHash() || !window->doc.readOnly.isEmpty())
        fail("保存したコピーを確認できません。元の文書は保持しています。");
    window->setObjectName("unprotectedCreatedDocument");
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->refresh(true);
    window->show();
    window->canvas->goToPage(0);
    window.release();
    status->setText("保護を解除した編集用コピーを、別のウィンドウで開きました。");
}
} // namespace tatsu
