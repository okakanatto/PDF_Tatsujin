#include "image_pdf_dialog.h"
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
} // namespace tatsu
