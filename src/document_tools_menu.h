#pragma once
#include <QMenu>

namespace tatsu
{
struct DocumentToolMenus
{
    QMenu* root;
    QMenu* create;
    QMenu* exportFiles;
    QMenu* edit;
    QMenu* protect;
    QMenu* verify;
    QMenu* formData;
    QMenu* process;
};

// QAction ownership stays with Qt; callers retain document state and callbacks.
inline DocumentToolMenus makeDocumentToolMenus(QWidget* owner)
{
    auto root = new QMenu(owner);
    root->setObjectName("documentToolsMenu");
    return {root,
            root->addMenu("PDFを作成"),
            root->addMenu("書き出し・変換"),
            root->addMenu("本文・ページの編集"),
            root->addMenu("保護・署名"),
            root->addMenu("検証・比較"),
            root->addMenu("フォーム入力データ"),
            root->addMenu("容量・一括処理")};
}
} // namespace tatsu
