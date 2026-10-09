#include "page_decoration_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::editPageDecoration(DecorationKind kind)
{
    doc.editable();
    canvas->finishFormEdit();
    const auto revision = doc.revision;
    PageDecorationDialog dialog(doc.pdf(), canvas->page, kind, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (revision != doc.revision)
        fail("作業中に文書が変わりました。設定を開き直してください。");
    doc.commit(dialog.takeDocument());
    refresh();
}
} // namespace tatsu
