#include "certificate_dialog.h"
#include "window.h"

namespace tatsu
{
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
