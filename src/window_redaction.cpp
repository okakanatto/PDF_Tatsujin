#include "redaction_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::exportRedactedCopy()
{
    doc.editable();
    canvas->finishFormEdit();
    const auto base = doc.target.isEmpty() ? doc.source : doc.target;
    const auto info = QFileInfo(base);
    const auto suggested =
        base.isEmpty() ? QString()
                       : info.dir().absoluteFilePath(info.completeBaseName() + "_墨消し.pdf");
    QVector<QPair<QString, QByteArray>> originals;
    if (!doc.source.isEmpty())
        originals << qMakePair(doc.source, doc.sourceHash);
    if (!doc.target.isEmpty() && !sameFilePath(doc.source, doc.target))
        originals << qMakePair(doc.target, doc.targetHash);
    RedactionDialog dialog(doc.pdf(), canvas->page, originals, suggested, this);
    if (dialog.exec() != QDialog::Accepted || dialog.savedPath().isEmpty())
        return;
    if (fileHash(dialog.savedPath()) != dialog.savedHash())
        fail("保存した墨消しコピーが更新されました。元の文書は保持しています。");
    auto window = std::make_unique<Window>();
    window->doc.open(dialog.savedPath());
    if (window->doc.sourceHash != dialog.savedHash())
        fail("墨消ししたコピーを開き直せません。元の文書は保持しています。");
    window->setObjectName("redactedCreatedDocument");
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->refresh(true);
    window->show();
    window->canvas->goToPage(qMin(canvas->page, window->doc.pages() - 1));
    window.release();
    status->setText("墨消ししたコピーを別ウィンドウで開きました。元文書とUndoは保持しています。");
}
} // namespace tatsu
