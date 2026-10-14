#include "bookmark_edit_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::editBookmarks()
{
    doc.editable();
    canvas->finishFormEdit();
    const auto revision = doc.revision;
    BookmarkEditDialog dialog(doc.pdf(), canvas->page, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (revision != doc.revision)
        fail("作業中に文書が変わりました。しおりの編集を開き直してください。");
    auto candidate = dialog.takeDocument();
    if (candidate != doc.pdf())
        doc.commit(std::move(candidate));
    refresh();
}
} // namespace tatsu
