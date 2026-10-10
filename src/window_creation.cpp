#include "image_export_dialog.h"
#include "image_pdf_dialog.h"
#include "office_import_dialog.h"
#include "pdfsecurityhandler.h"
#include "table_extraction_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::createFromDocx(QString path)
{
    createFromOffice(std::move(path));
}
void Window::createFromOffice(QString path)
{
    if (path.isEmpty())
        path = QFileDialog::getOpenFileName(this, "PDFにするOffice文書を選ぶ", {},
                                            "Office文書 (*.docx *.xlsx *.pptx)");
    if (path.isEmpty())
        return;
    OfficeImportDialog dialog(path, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto window = new Window;
    window->setObjectName("officeCreatedDocument");
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->doc.history = {dialog.takeDocument()};
    window->doc.saved = -1;
    ++window->doc.revision;
    window->refresh(true);
    window->show();
    window->canvas->goToPage(0);
}
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
void Window::extractDocumentTable()
{
    if (!doc.loaded() || !doc.pdf().getStorage().getSecurityHandler()->isAllowed(
                             PDFSecurityHandler::Permission::CopyContent))
        fail("この文書では表の文字を取り出せません。");
    canvas->finishFormEdit();
    const auto revision = doc.revision;
    TableExtractionDialog dialog(
        doc.pdf(), canvas->page,
        [this, revision]
        {
            if (doc.revision != revision)
                fail("元のPDFが変更されました。表の取り出しを開き直してください。");
        },
        this);
    dialog.exec();
}
} // namespace tatsu
