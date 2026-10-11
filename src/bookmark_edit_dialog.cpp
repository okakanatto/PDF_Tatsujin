#include "bookmark_edit_dialog.h"

namespace tatsu
{
namespace
{
int descendants(QTreeWidgetItem* item)
{
    int count = 0;
    for (int i = 0; i < item->childCount(); ++i)
        count += 1 + descendants(item->child(i));
    return count;
}
QTreeWidgetItem* take(QTreeWidget* tree, QTreeWidgetItem* item)
{
    return item->parent() ? item->parent()->takeChild(item->parent()->indexOfChild(item))
                          : tree->takeTopLevelItem(tree->indexOfTopLevelItem(item));
}
int subtreeHeight(QTreeWidgetItem* item)
{
    int result = 1;
    for (int i = 0; i < item->childCount(); ++i)
        result = qMax(result, 1 + subtreeHeight(item->child(i)));
    return result;
}
} // namespace
BookmarkEditDialog::BookmarkEditDialog(PDFDocument document, int currentPage, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), current(currentPage)
{
    setObjectName("bookmarkEditDialog");
    setWindowTitle("しおりを編集");
    resize(800, 550);
    setMinimumSize(760, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("候補の一覧を編集し、最後に文書へ適用します。既存の移動先・表示位置は、"
                           "「移動先を変更」を選ぶまで保持します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    auto buttons = new QHBoxLayout;
    auto button = [&](QString text, QString name, const std::function<void()>& operation)
    {
        auto widget = new QPushButton(text);
        widget->setObjectName(name);
        connect(widget, &QPushButton::clicked, this, operation);
        buttons->addWidget(widget);
        return widget;
    };
    button("追加", "bookmarkAdd", [this] { add(); });
    remove = button("選択を削除", "bookmarkDelete", [this] { erase(); });
    button("↑ 上へ", "bookmarkUp", [this] { move(-1); });
    button("↓ 下へ", "bookmarkDown", [this] { move(1); });
    button("階層を深く", "bookmarkIndent", [this] { indent(true); });
    button("階層を浅く", "bookmarkOutdent", [this] { indent(false); });
    settingsLayout->addLayout(buttons);
    auto columns = new QHBoxLayout;
    tree = new QTreeWidget;
    tree->setObjectName("bookmarkEditTree");
    tree->setAccessibleName("編集するしおりの階層と移動先");
    tree->setHeaderLabels({"しおりの名前", "移動先"});
    tree->setColumnCount(2);
    tree->setUniformRowHeights(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    columns->addWidget(tree, 1);
    auto editor = new QWidget;
    editor->setMaximumWidth(300);
    auto form = new QFormLayout(editor);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    title = new QLineEdit;
    title->setObjectName("bookmarkTitle");
    title->setMaxLength(200);
    form->addRow("名前", title);
    destination = new QCheckBox("移動先を変更（ページ全体表示）");
    destination->setObjectName("bookmarkChangeDestination");
    form->addRow(destination);
    page = new QSpinBox;
    page->setObjectName("bookmarkDestinationPage");
    page->setRange(1, int(snapshot.getCatalog()->getPageCount()));
    page->setSuffix(" ページ");
    form->addRow("物理ページ番号", page);
    auto hint = new QLabel("上下移動は子を含む枝全体を動かします。\n"
                           "「階層を深く」は直前の兄弟の子にします。\n"
                           "矢印で開閉した状態も保存します。");
    hint->setWordWrap(true);
    form->addRow(hint);
    columns->addWidget(editor);
    settingsLayout->addLayout(columns, 1);
    layout->addWidget(settings, 1);
    message = new QLabel("この一覧だけを編集しています。元の文書にはまだ反映していません。");
    message->setObjectName("bookmarkEditMessage");
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    apply = actions->addButton("文書へ適用", QDialogButtonBox::AcceptRole);
    apply->setObjectName("bookmarkApply");
    apply->setProperty("primary", true);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("bookmarkCancel");
    layout->addWidget(actions);
    values = editableBookmarks(snapshot);
    QVector<QTreeWidgetItem*> rows;
    for (int i = 0; i < values.size(); ++i)
    {
        const auto& value = values[i];
        auto item =
            value.parent < 0 ? new QTreeWidgetItem(tree) : new QTreeWidgetItem(rows[value.parent]);
        rows << item;
        item->setData(0, Qt::UserRole, i);
        item->setText(0, value.title);
        item->setText(1, value.page >= 0 ? QString("%1ページ（保持）").arg(value.page + 1)
                                         : "元の宛先を保持");
        item->setExpanded(value.expanded);
    }
    connect(tree, &QTreeWidget::currentItemChanged, this, [this] { select(); });
    connect(title, &QLineEdit::textChanged, this, [this] { update(); });
    connect(destination, &QCheckBox::toggled, this, [this] { update(); });
    connect(page, &QSpinBox::valueChanged, this, [this] { update(); });
    connect(apply, &QPushButton::clicked, this, &BookmarkEditDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &BookmarkEditDialog::reject);
    if (!rows.isEmpty())
        tree->setCurrentItem(rows.front());
    else
        select();
}
BookmarkEditDialog::~BookmarkEditDialog()
{
    cancelled = true;
    if (job)
        job->wait();
}
void BookmarkEditDialog::select()
{
    auto item = tree->currentItem();
    loading = true;
    title->setEnabled(item);
    destination->setEnabled(item);
    remove->setEnabled(item);
    page->setEnabled(false);
    if (item)
    {
        const auto& value = values.at(item->data(0, Qt::UserRole).toInt());
        title->setText(value.title);
        destination->setChecked(value.changeDestination);
        page->setValue(value.page >= 0 ? value.page + 1 : current + 1);
        page->setEnabled(value.changeDestination);
        remove->setText(QString("選択と子を削除（%1項目）").arg(descendants(item) + 1));
    }
    else
    {
        title->clear();
        destination->setChecked(false);
        remove->setText("選択を削除");
    }
    loading = false;
}
void BookmarkEditDialog::update()
{
    if (loading || job || !tree->currentItem())
        return;
    message->setText("候補を編集しています。文書へ適用して確定できます。");
    auto item = tree->currentItem();
    auto& value = values[item->data(0, Qt::UserRole).toInt()];
    value.title = title->text();
    value.changeDestination = destination->isChecked();
    if (value.changeDestination)
        value.page = page->value() - 1;
    page->setEnabled(value.changeDestination);
    item->setText(0, value.title);
    item->setText(1, value.changeDestination ? QString("%1ページ（変更）").arg(value.page + 1)
                                             : "元の宛先を保持");
}
void BookmarkEditDialog::add()
{
    if (entries().size() >= 1000)
    {
        message->setText("しおりは1000項目までです。既存の一覧は保持しています。");
        return;
    }
    const int index = values.size();
    values << BookmarkEntry{{}, "新しいしおり", -1, current, true, true};
    auto selected = tree->currentItem();
    auto parent = selected ? selected->parent() : nullptr;
    auto item = new QTreeWidgetItem;
    item->setData(0, Qt::UserRole, index);
    item->setText(0, values.back().title);
    item->setText(1, QString("%1ページ（新規）").arg(current + 1));
    if (parent)
        parent->insertChild(parent->indexOfChild(selected) + 1, item);
    else
        tree->insertTopLevelItem(
            selected ? tree->indexOfTopLevelItem(selected) + 1 : tree->topLevelItemCount(), item);
    tree->setCurrentItem(item);
    tree->scrollToItem(item);
    title->setFocus();
    title->selectAll();
}
void BookmarkEditDialog::erase()
{
    auto item = tree->currentItem();
    if (!item)
        return;
    delete take(tree, item);
    select();
}
void BookmarkEditDialog::move(int offset)
{
    auto item = tree->currentItem();
    if (!item)
        return;
    auto parent = item->parent();
    const int index = parent ? parent->indexOfChild(item) : tree->indexOfTopLevelItem(item);
    const int target = index + offset,
              count = parent ? parent->childCount() : tree->topLevelItemCount();
    if (target < 0 || target >= count)
        return;
    take(tree, item);
    if (parent)
        parent->insertChild(target, item);
    else
        tree->insertTopLevelItem(target, item);
    tree->setCurrentItem(item);
}
void BookmarkEditDialog::indent(bool deeper)
{
    auto item = tree->currentItem();
    if (!item)
        return;
    auto parent = item->parent();
    if (deeper)
    {
        const int index = parent ? parent->indexOfChild(item) : tree->indexOfTopLevelItem(item);
        if (index == 0)
            return;
        auto previous = parent ? parent->child(index - 1) : tree->topLevelItem(index - 1);
        int depth = 0;
        for (auto ancestor = previous; ancestor; ancestor = ancestor->parent())
            ++depth;
        if (depth + subtreeHeight(item) > 32)
        {
            message->setText("しおりは32階層までです。既存の一覧は保持しています。");
            return;
        }
        take(tree, item);
        previous->addChild(item);
        previous->setExpanded(true);
    }
    else
    {
        if (!parent)
            return;
        auto grandparent = parent->parent();
        const int index =
            grandparent ? grandparent->indexOfChild(parent) : tree->indexOfTopLevelItem(parent);
        take(tree, item);
        if (grandparent)
            grandparent->insertChild(index + 1, item);
        else
            tree->insertTopLevelItem(index + 1, item);
    }
    tree->setCurrentItem(item);
}
QVector<BookmarkEntry> BookmarkEditDialog::entries() const
{
    QVector<BookmarkEntry> result;
    std::function<void(QTreeWidgetItem*, int)> append = [&](QTreeWidgetItem* item, int parent)
    {
        auto value = values.at(item->data(0, Qt::UserRole).toInt());
        value.parent = parent;
        value.expanded = item->isExpanded();
        const int index = result.size();
        result << value;
        for (int i = 0; i < item->childCount(); ++i)
            append(item->child(i), index);
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        append(tree->topLevelItem(i), -1);
    return result;
}
void BookmarkEditDialog::accept()
{
    if (job)
        return;
    const auto edited = entries();
    settings->setEnabled(false);
    apply->setEnabled(false);
    cancelled = false;
    message->setText("しおりの候補を確認しています。元の文書は変更していません。");
    struct Result
    {
        std::optional<PDFDocument> document;
        QString error;
    };
    auto result = std::make_shared<Result>();
    job = QThread::create(
        [this, document = snapshot, edited, result]
        {
            try
            {
                result->document =
                    replaceBookmarks(document, edited, [this] { return cancelled.load(); });
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "しおりを変更できません。元の文書は保持しています。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, result]
            {
                job = nullptr;
                launched->deleteLater();
                if (cancelled)
                    QDialog::reject();
                else if (result->document)
                {
                    candidate = std::move(result->document);
                    QDialog::accept();
                }
                else
                {
                    settings->setEnabled(true);
                    apply->setEnabled(true);
                    message->setText(result->error);
                }
            });
    launched->start();
}
void BookmarkEditDialog::reject()
{
    if (job)
    {
        cancelled = true;
        cancel->setEnabled(false);
        message->setText("中止しています。元の文書は変更していません。");
    }
    else
        QDialog::reject();
}
void BookmarkEditDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
PDFDocument BookmarkEditDialog::takeDocument()
{
    if (!candidate || job || result() != QDialog::Accepted)
        fail("完成したしおりの候補がありません。");
    auto result = std::move(*candidate);
    candidate.reset();
    return result;
}
} // namespace tatsu
