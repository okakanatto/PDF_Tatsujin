#include "page_organizer.h"
#include "pdfdocumentbuilder.h"
#include <QtWidgets>

namespace tatsu
{
void OrganizerPreviews::dropEvent(QDropEvent* event)
{
    if (event->source() != this)
    {
        event->ignore();
        return;
    }
    QVector<int> selection;
    for (auto item : selectedItems())
        selection.append(row(item));
    const auto target = indexAt(event->position().toPoint());
    int before = target.isValid() ? target.row() : count();
    if (target.isValid() && event->position().x() > visualRect(target).center().x())
        ++before;
    if (reorder)
        reorder(movePageOrder(count(), selection, before));
    event->acceptProposedAction();
}
PageOrganizer::PageOrganizer(Document* doc, QWidget* parent) : QWidget(parent), document(doc)
{
    setObjectName("pageOrganizer");
    auto layout = new QVBoxLayout(this);
    auto row = new QHBoxLayout;
    auto title = new QLabel("ページ整理");
    title->setProperty("sectionTitle", true);
    auto back = new QPushButton("文書に戻る");
    back->setObjectName("returnFromOrganizer");
    row->addWidget(title);
    row->addStretch();
    row->addWidget(back);
    layout->addLayout(row);
    connect(back, &QPushButton::clicked, this,
            [this]
            {
                if (returnToDocument)
                    returnToDocument();
            });
    previews = new OrganizerPreviews;
    previews->setObjectName("organizerPages");
    previews->setViewMode(QListView::IconMode);
    previews->setResizeMode(QListView::Adjust);
    previews->setMovement(QListView::Snap);
    previews->setGridSize({176, 180});
    previews->setSelectionMode(QAbstractItemView::ExtendedSelection);
    previews->setDragDropMode(QAbstractItemView::InternalMove);
    previews->setDefaultDropAction(Qt::MoveAction);
    previews->setAccessibleName("ページ整理。Ctrl・Shiftで複数選択、ドラッグで順序変更");
    layout->addWidget(previews);
    auto hint = new QLabel("Ctrl・Shiftで複数選択。ドラッグして順序を変更できます。");
    layout->addWidget(hint);
    options = new QWidget;
    options->setObjectName("organizerSettings");
    auto settings = new QVBoxLayout(options);
    settings->setContentsMargins(18, 16, 18, 18);
    summary = new QLabel;
    summary->setWordWrap(true);
    settings->addWidget(summary);
    ranges = new QLineEdit;
    ranges->setObjectName("organizerRange");
    ranges->setPlaceholderText("物理ページ番号 例: 1,3-5");
    ranges->setAccessibleName("整理・出力する物理ページ範囲");
    settings->addWidget(ranges);
    auto selectRange = new QPushButton("範囲を選択");
    settings->addWidget(selectRange);
    connect(selectRange, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        auto pages = parsePages(ranges->text(), document->pages());
                        previews->clearSelection();
                        for (int page : pages)
                            previews->item(page)->setSelected(true);
                    });
            });
    auto moves = new QHBoxLayout;
    auto earlier = new QPushButton("前へ");
    auto later = new QPushButton("後ろへ");
    moves->addWidget(earlier);
    moves->addWidget(later);
    settings->addLayout(moves);
    connect(earlier, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        auto pages = selected();
                        applyOrder(
                            movePageOrder(document->pages(), pages, qMax(0, pages.front() - 1)));
                    });
            });
    connect(later, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        auto pages = selected();
                        applyOrder(movePageOrder(document->pages(), pages,
                                                 qMin(document->pages(), pages.back() + 2)));
                    });
            });
    before = new QSpinBox;
    before->setAccessibleName("移動先。指定した物理ページの前へ移動");
    before->setPrefix("移動先: ");
    before->setSuffix(" ページの前");
    settings->addWidget(before);
    auto move = new QPushButton("指定位置へ移動");
    settings->addWidget(move);
    connect(
        move, &QPushButton::clicked, this,
        [this] {
            guard(
                [&]
                { applyOrder(movePageOrder(document->pages(), selected(), before->value() - 1)); });
        });
    auto changes = new QHBoxLayout;
    auto rotate = new QPushButton("右へ90°回転");
    auto remove = new QPushButton("削除");
    remove->setObjectName("deleteOrganizerPages");
    rotate->setObjectName("rotateOrganizerPages");
    changes->addWidget(rotate);
    changes->addWidget(remove);
    settings->addLayout(changes);
    connect(rotate, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        document->editable();
                        auto selection = selected();
                        PDFDocumentBuilder builder(&document->pdf());
                        for (int page : selection)
                        {
                            auto p = document->pdf().getCatalog()->getPage(page);
                            builder.setPageRotation(
                                p->getPageReference(),
                                getPageRotationRotatedRight(p->getPageRotation()));
                        }
                        document->commit(builder.build());
                        if (changed)
                            changed({});
                    });
            });
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        const auto selection = selected();
                        QVector<int> order;
                        for (int i = 0; i < document->pages(); ++i)
                            if (!selection.contains(i))
                                order.append(i);
                        if (order.isEmpty())
                            fail("最後の1ページは削除できません。");
                        if (QMessageBox::question(
                                this, "ページを削除",
                                QString("選択した%1ページを削除しますか？ Undoで戻せます。")
                                    .arg(selection.size())) == QMessageBox::Yes)
                            applyOrder(order);
                    });
            });
    insertion = new QComboBox;
    insertion->addItems({"選択ページの前へ挿入", "選択ページの後へ挿入", "末尾へ挿入"});
    settings->addWidget(insertion);
    auto insertButton = new QPushButton("別PDFのページを挿入");
    settings->addWidget(insertButton);
    connect(insertButton, &QPushButton::clicked, this, [this] { guard([&] { insert(); }); });
    auto exports = new QToolButton;
    exports->setText("抽出・分割して保存");
    exports->setToolButtonStyle(Qt::ToolButtonTextOnly);
    exports->setPopupMode(QToolButton::InstantPopup);
    auto exportMenu = new QMenu(exports);
    exportMenu->addAction("選択ページを1つのPDFへ抽出", this,
                          [this] { guard([&] { extract(false); }); });
    exportMenu->addAction("範囲欄の各範囲で分割", this, [this] { guard([&] { extract(true); }); });
    exportMenu->addAction("選択ページを1ページずつ分割", this,
                          [this] { guard([&] { extract(true, true); }); });
    exports->setMenu(exportMenu);
    settings->addWidget(exports);
    auto numbering =
        new QLabel("番号は先頭からの物理ページ番号です。末尾への移動は総ページ数＋1を指定します。");
    numbering->setWordWrap(true);
    settings->addWidget(numbering);
    settings->addStretch();
    connect(previews, &QListWidget::itemSelectionChanged, this,
            [this]
            {
                summary->setText(QString("%1ページ中 %2ページを選択")
                                     .arg(document->pages())
                                     .arg(previews->selectedItems().size()));
            });
    previews->reorder = [this](QVector<int> order) { guard([&] { applyOrder(order); }); };
}
void PageOrganizer::guard(const std::function<void()>& operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        refresh();
        QMessageBox::warning(this, "ページ操作を完了できません", QString::fromUtf8(error.what()));
    }
}
void PageOrganizer::refresh()
{
    QSignalBlocker blocker(previews);
    QVector<int> selection;
    for (auto item : previews->selectedItems())
        selection.append(item->data(PagePreviews::StatusRole + 1).toInt());
    previews->clear();
    for (int page = 0; page < document->pages(); ++page)
    {
        previews->addItem(QString("%1ページ").arg(page + 1));
        auto item = previews->item(page);
        item->setData(PagePreviews::StatusRole + 1, page);
        item->setSelected(selection.contains(page));
    }
    previews->setDocument(document->loaded() ? &document->pdf() : nullptr, document->revision);
    before->setRange(1, document->pages() + 1);
    summary->setText(QString("%1ページ中 %2ページを選択")
                         .arg(document->pages())
                         .arg(previews->selectedItems().size()));
    options->setEnabled(document->loaded() && document->readOnly.isEmpty() && !document->busy);
    previews->setDragEnabled(options->isEnabled());
}
QVector<int> PageOrganizer::selected() const
{
    QVector<int> selection;
    for (auto item : previews->selectedItems())
        selection.append(previews->row(item));
    std::sort(selection.begin(), selection.end());
    if (selection.isEmpty())
        fail("ページを選択してください。");
    return selection;
}
void PageOrganizer::applyOrder(const QVector<int>& order)
{
    document->editable();
    QVector<int> identity;
    for (int i = 0; i < document->pages(); ++i)
        identity.append(i);
    if (order == identity)
    {
        refresh();
        return;
    }
    document->commit(selectPages(document->pdf(), order));
    if (changed)
        changed(order);
}
void PageOrganizer::insert()
{
    document->editable();
    const auto path = QFileDialog::getOpenFileName(this, "挿入するPDF", {}, "PDF (*.pdf)");
    if (path.isEmpty())
        return;
    Document incoming;
    incoming.open(path);
    incoming.editable();
    bool accepted = false;
    auto range = QInputDialog::getText(this, "挿入するページ", "物理ページ範囲", QLineEdit::Normal,
                                       QString("1-%1").arg(incoming.pages()), &accepted);
    if (!accepted)
        return;
    int position = document->pages();
    if (insertion->currentIndex() != 2 && !previews->selectedItems().isEmpty())
    {
        auto pages = selected();
        position = insertion->currentIndex() == 0 ? pages.front() : pages.back() + 1;
    }
    const auto selectedPages = parsePages(range, incoming.pages());
    auto candidate = insertPages(document->pdf(), incoming.pdf(), selectedPages, position);
    QVector<int> order;
    for (int i = 0; i <= document->pages(); ++i)
    {
        if (i == position)
            for (int page : selectedPages)
            {
                Q_UNUSED(page);
                order.append(-1);
            }
        if (i < document->pages())
            order.append(i);
    }
    document->commit(std::move(candidate));
    if (changed)
        changed(order);
}
void PageOrganizer::extract(bool split, bool eachPage)
{
    document->editable();
    QVector<QVector<int>> groups;
    if (!split)
        groups.append(selected());
    else if (eachPage)
        for (int page : selected())
            groups.append({page});
    else
        for (const auto& part : ranges->text().split(','))
            groups.append(parsePages(part, document->pages()));
    QStringList paths;
    if (split)
    {
        const auto directory = QFileDialog::getExistingDirectory(this, "分割PDFの保存先");
        if (directory.isEmpty())
            return;
        const auto stem =
            QFileInfo(document->target.isEmpty() ? document->source : document->target)
                .completeBaseName();
        for (int i = 0; i < groups.size(); ++i)
            paths.append(QDir(directory).filePath(QString("%1-%2.pdf")
                                                      .arg(stem.isEmpty() ? "分割" : stem)
                                                      .arg(i + 1, 3, 10, QChar('0'))));
    }
    else
    {
        const auto path = QFileDialog::getSaveFileName(this, "抽出PDFを保存", {}, "PDF (*.pdf)",
                                                       nullptr, QFileDialog::DontConfirmOverwrite);
        if (path.isEmpty())
            return;
        paths.append(path.endsWith(".pdf", Qt::CaseInsensitive) ? path : path + ".pdf");
    }
    const auto results = exportPageGroups(document->pdf(), groups, paths);
    QStringList lines;
    for (const auto& result : results)
        lines.append((result.success ? "保存済み: " : "未保存: ") + result.path +
                     (result.error.isEmpty() ? "" : "\n" + result.error));
    QMessageBox message(QMessageBox::Information, "抽出・分割の結果", lines.join("\n\n"),
                        QMessageBox::Ok, this);
    message.setTextFormat(Qt::PlainText);
    message.exec();
}
} // namespace tatsu
