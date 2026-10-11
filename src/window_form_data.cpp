#include "form_data_dialog.h"
#include "window.h"

namespace tatsu
{
void Window::manageFormData(bool importing)
{
    if (!doc.loaded() || doc.busy)
        fail("処理中でないPDFを開いてください。");
    if (importing)
        doc.editable();
    canvas->finishFormEdit();
    const auto revision = doc.revision;
    const auto base = doc.target.isEmpty() ? doc.source : doc.target;
    const auto suggested =
        importing || base.isEmpty()
            ? QString()
            : QFileInfo(base).dir().filePath(QFileInfo(base).completeBaseName() + "-values.xfdf");
    FormDataDialog dialog(doc.pdf(), importing, suggested, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (importing)
    {
        if (doc.revision != revision)
            fail("確認中に文書が変更されました。読み込み直してください。");
        auto result = dialog.candidate();
        if (!result.changes.isEmpty())
        {
            doc.commit(std::move(result.document));
            refresh(true);
            status->setText(QString("%1項目の入力値を読み込みました。Undoで戻せます。")
                                .arg(result.changes.size()));
        }
    }
    else
        status->setText("入力値を書き出しました: " + QFileInfo(dialog.savedPath()).fileName());
}
} // namespace tatsu
