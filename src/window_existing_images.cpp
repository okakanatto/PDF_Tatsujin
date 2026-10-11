#include "existing_image_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::editExistingImages()
{
    doc.editable();
    canvas->finishFormEdit();
    const auto revision = doc.revision;
    QVector<QPair<QString, QByteArray>> originals;
    if (!doc.source.isEmpty())
        originals << qMakePair(doc.source, doc.sourceHash);
    if (!doc.target.isEmpty() && !sameFilePath(doc.source, doc.target))
        originals << qMakePair(doc.target, doc.targetHash);
    auto validate = [originals]
    {
        for (const auto& file : originals)
            if (fileHash(file.first) != file.second)
                fail("元のPDFが更新されました。画像の変更は適用していません。");
    };
    ExistingImageDialog dialog(doc.pdf(), canvas->page, validate, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    validate();
    if (doc.revision != revision)
        fail("作業中に文書が変わりました。画像編集を開き直してください。");
    doc.commit(dialog.takeDocument());
    refresh();
}
} // namespace tatsu
