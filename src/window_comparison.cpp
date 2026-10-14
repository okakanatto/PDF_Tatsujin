#include "comparison_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::compareWithDocument()
{
    if (!doc.loaded() || !doc.copyAllowed || doc.busy)
        fail("コピーが許可されたPDFを開き、処理が終わってから比較してください。");
    canvas->finishFormEdit();
    ComparisonDialog dialog(doc.pdf(), doc.source, doc.sourceHash, this);
    dialog.exec();
}
} // namespace tatsu
