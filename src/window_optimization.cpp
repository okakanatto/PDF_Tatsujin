#include "pdf_optimization_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::optimizeDocument()
{
    doc.editable();
    canvas->finishFormEdit();
    const auto revision = doc.revision;
    PdfOptimizationDialog dialog(doc.pdf(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (revision != doc.revision)
        fail("作業中に文書が変わりました。容量最適化を開き直してください。");
    doc.commit(dialog.takeDocument());
    refresh(true);
}
} // namespace tatsu
