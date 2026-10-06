#include "window.h"

namespace tatsu
{
static QString libraryPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/signatures.json";
}
void Window::setupWriting()
{
    writingPanel = new WritingPanel(&doc, canvas, false);
    imageSignaturePanel = new WritingPanel(&doc, canvas, true);
    panels->addWidget(writingPanel);
    panels->addWidget(imageSignaturePanel);
    for (auto panel : {writingPanel, imageSignaturePanel})
    {
        panel->requestPlacement = [this]
        {
            canvas->beginPlacement();
            status->setText("配置位置をクリック。Escで解除。");
        };
        panel->changed = [this](PDFObjectReference reference) { refresh(false, reference); };
        panel->saveTemplate = [this](SignatureTemplate item)
        { saveSignatureTemplate(std::move(item)); };
    }
}
void Window::saveSignatureTemplate(SignatureTemplate item)
{
    if (isImage(item.kind) ? item.image.isNull() : item.text.trimmed().isEmpty())
        fail("保存する署名を指定してください。");
    bool accepted = false;
    const auto name = QInputDialog::getText(this, "署名をこのPCに保存", "一覧に表示する名前",
                                            QLineEdit::Normal, {}, &accepted);
    if (!accepted)
        return;
    item.name = name.trimmed();
    SignatureLibrary(libraryPath()).add(std::move(item));
    progress->setText("署名をこのPCに保存しました。");
}
void Window::openSignatureLibrary()
{
    SignatureLibrary library(libraryPath());
    auto items = library.load();
    QDialog dialog(this);
    dialog.setWindowTitle("このPCの保存済み署名");
    dialog.resize(440, 360);
    auto layout = new QVBoxLayout(&dialog);
    auto note = new QLabel("ここから削除しても、PDFへ配置済みの署名は変わりません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto list = new QListWidget;
    layout->addWidget(list);
    auto fill = [&]
    {
        list->clear();
        for (const auto& item : items)
            list->addItem(item.name + (isImage(item.kind) ? " — 画像" : " — テキスト"));
        if (!items.isEmpty())
            list->setCurrentRow(0);
    };
    fill();
    auto row = new QHBoxLayout;
    auto remove = new QPushButton("一覧から削除");
    auto place = new QPushButton("この署名を配置");
    place->setProperty("primary", true);
    auto close = new QPushButton("閉じる");
    row->addWidget(remove);
    row->addWidget(place);
    row->addWidget(close);
    layout->addLayout(row);
    auto selectedChanged = [&]
    {
        const bool valid = list->currentRow() >= 0;
        remove->setEnabled(valid);
        place->setEnabled(valid && doc.loaded() && doc.readOnly.isEmpty() && !doc.busy);
    };
    connect(list, &QListWidget::currentRowChanged, &dialog, selectedChanged);
    selectedChanged();
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(remove, &QPushButton::clicked, &dialog,
            [&]
            {
                guard(
                    [&]
                    {
                        const int index = list->currentRow();
                        if (index < 0)
                            return;
                        if (QMessageBox::question(&dialog, "署名を削除",
                                                  "選択した保存済み署名を一覧から削除しますか？") !=
                            QMessageBox::Yes)
                            return;
                        library.remove(items[index].id);
                        items = library.load();
                        fill();
                        selectedChanged();
                    });
            });
    connect(place, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted || list->currentRow() < 0)
        return;
    const auto item = items[list->currentRow()];
    if (isImage(item.kind))
    {
        imageSignaturePanel->setTemplate(item);
        showPanel(3);
    }
    else
    {
        signature->setPlainText(item.text);
        size->setValue(item.size);
        ink = item.color;
        showPanel(0);
    }
    canvas->beginPlacement();
    refreshStatus();
}
} // namespace tatsu
