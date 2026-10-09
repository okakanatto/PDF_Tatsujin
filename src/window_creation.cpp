#include "image_export_dialog.h"
#include "image_pdf_dialog.h"
#include "pdfsecurityhandler.h"
#include "window.h"

namespace tatsu
{
void Window::createFromImages(QStringList paths)
{
    if (paths.isEmpty())
        paths = QFileDialog::getOpenFileNames(this, "PDFにする画像を選ぶ", {},
                                              "PNG・JPEG (*.png *.jpg *.jpeg)");
    if (paths.isEmpty())
        return;
    ImagePdfDialog dialog(paths, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto candidate = dialog.takeDocument();
    auto window = new Window;
    window->setObjectName("imageCreatedDocument");
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->doc.history = {std::move(candidate)};
    window->doc.saved = -1;
    ++window->doc.revision;
    window->refresh(true);
    window->show();
    window->canvas->goToPage(0);
}
void Window::exportDocumentImages()
{
    if (!doc.loaded() || !doc.pdf().getStorage().getSecurityHandler()->isAllowed(
                             pdf::PDFSecurityHandler::Permission::CopyContent))
        fail("この文書では画像の出力が許可されていません。");
    canvas->finishFormEdit();
    const auto name = QFileInfo(doc.target.isEmpty() ? doc.source : doc.target).completeBaseName();
    ImageExportDialog dialog(doc.pdf(), canvas->page, name, this);
    dialog.exec();
}
} // namespace tatsu
