#include "bookmarks_panel.h"

namespace tatsu
{
BookmarksPanel::BookmarksPanel(QWidget* parent) : QWidget(parent)
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    edit = new QPushButton("しおりを編集…");
    edit->setObjectName("editDocumentBookmarks");
    edit->setEnabled(false);
    layout->addWidget(edit);
    connect(edit, &QPushButton::clicked, this,
            [this]
            {
                if (editRequested)
                    editRequested();
            });
    notice = new QLabel;
    notice->setWordWrap(true);
    tree = new QTreeWidget;
    tree->setObjectName("bookmarkTree");
    tree->setAccessibleName("文書のしおり。クリックまたはEnterで移動");
    tree->setHeaderHidden(true);
    tree->setColumnCount(1);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(QHeaderView::Stretch);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->setTextElideMode(Qt::ElideRight);
    tree->setUniformRowHeights(true);
    tree->setIndentation(16);
    layout->addWidget(notice);
    layout->addWidget(tree);
    connect(tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item) { activate(item); });
    connect(tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item) { activate(item); });
}
void BookmarksPanel::setEditable(bool value)
{
    edit->setEnabled(value);
}
void BookmarksPanel::activate(QTreeWidgetItem* item)
{
    if (activated && targets.contains(item))
    {
        const auto target = targets.value(item);
        activated(target);
    }
}
void BookmarksPanel::reset()
{
    tree->clear();
    targets.clear();
    version = std::numeric_limits<quint64>::max();
}
void BookmarksPanel::setDocument(const PDFDocument* document, quint64 revision,
                                 const QStringList& labels)
{
    if (version == revision)
        return;
    const bool first = tree->topLevelItemCount() == 0;
    version = revision;
    QSet<int> expanded;
    int current = -1;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
    {
        const auto id = (*it)->data(0, Qt::UserRole).toInt();
        if ((*it)->isExpanded())
            expanded.insert(id);
        if (*it == tree->currentItem())
            current = id;
    }
    const int scroll = tree->verticalScrollBar()->value();
    tree->clear();
    targets.clear();
    int count = 0;
    const auto bookmarks = document ? readBookmarks(*document) : BookmarkList();
    QVector<QTreeWidgetItem*> rows;
    for (const auto& bookmark : bookmarks.items)
    {
        auto parent = bookmark.parent < 0 ? nullptr : rows[bookmark.parent];
        auto item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        rows.append(item);
        item->setData(0, Qt::UserRole, count);
        const auto& target = bookmark.target;
        item->setText(0, bookmark.title.isEmpty() ? "（無題のしおり）" : bookmark.title);
        const auto destination =
            target.valid() ? pageDescription(target.page, labels) : target.notice;
        item->setToolTip(0, Qt::convertFromPlainText(item->text(0) + "\n" + destination));
        item->setData(0, Qt::AccessibleDescriptionRole, destination);
        if (!target.valid())
            item->setForeground(0, QColor("#657185"));
        targets.insert(item, target);
        item->setExpanded(expanded.contains(count) || (first && bookmark.expanded));
        if (current == count)
            tree->setCurrentItem(item);
        ++count;
    }
    notice->setText(bookmarks.limited ? "しおりの一部は表示上限を超えています（1万件／64階層）。"
                    : count ? "クリック・Enterで移動。前の表示で読書位置へ戻れます。"
                            : "このPDFにはしおりがありません。");
    tree->setVisible(count > 0);
    tree->verticalScrollBar()->setValue(scroll);
}
} // namespace tatsu
