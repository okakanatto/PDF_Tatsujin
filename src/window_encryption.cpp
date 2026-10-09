#include "encryption_dialog.h"
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
} // namespace tatsu
