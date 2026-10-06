#include "window.h"

namespace tatsu
{
void Window::setupOrganizer()
{
    organizer = new PageOrganizer(&doc);
    documentArea->addWidget(organizer);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(organizer->settings());
    panels->addWidget(scroll);
    organizer->returnToDocument = [this] { setOrganizing(false); };
    organizer->changed = [this](QVector<int> order)
    {
        if (!order.isEmpty())
        {
            const int oldPage = organizerReadingState.anchor.page;
            auto newPage = order.indexOf(oldPage);
            if (newPage < 0)
                newPage = qBound(0, oldPage, doc.pages() - 1);
            organizerReadingState.anchor.page = newPage;
            canvas->page = newPage;
        }
        refresh(true);
    };
}
void Window::setOrganizing(bool enabled)
{
    if (enabled == organizing)
        return;
    if (enabled)
    {
        doc.editable();
        organizerReadingState = canvas->viewState();
        organizing = true;
        organizeAction->setChecked(true);
        showPanel(4);
        documentArea->setCurrentIndex(2);
        organizer->refresh();
        if (canvas->page >= 0 && canvas->page < organizer->previews->count())
            organizer->previews->item(canvas->page)->setSelected(true);
        organizer->previews->setFocus();
    }
    else
    {
        organizing = false;
        organizeAction->setChecked(false);
        documentArea->setCurrentIndex(doc.loaded() ? 1 : 0);
        properties->hide();
        syncReadingLayout();
        canvas->restoreView(organizerReadingState);
        preserveLayoutAnchor(organizerReadingState.anchor);
        canvas->setFocus();
    }
}
void Window::mergeFiles(QStringList paths)
{
    if (paths.isEmpty())
        paths = QFileDialog::getOpenFileNames(this, "結合するPDF", {}, "PDF (*.pdf)");
    if (paths.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("PDFの結合順");
    dialog.resize(520, 430);
    auto layout = new QVBoxLayout(&dialog);
    auto note = new QLabel(
        "一覧の順で新しいPDFを作ります。入力元は変更しません。ドラッグで順序を変えられます。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto list = new QListWidget;
    list->setDragDropMode(QAbstractItemView::InternalMove);
    layout->addWidget(list);
    auto append = [&](const QStringList& added)
    {
        for (const auto& path : added)
        {
            auto item = new QListWidgetItem(QFileInfo(path).fileName(), list);
            item->setData(Qt::UserRole, path);
            item->setToolTip(Qt::convertFromPlainText(path));
        }
    };
    append(paths);
    auto row = new QHBoxLayout;
    auto add = new QPushButton("PDFを追加");
    auto remove = new QPushButton("一覧から除く");
    row->addWidget(add);
    row->addWidget(remove);
    layout->addLayout(row);
    connect(add, &QPushButton::clicked, &dialog,
            [&] {
                append(
                    QFileDialog::getOpenFileNames(&dialog, "結合するPDFを追加", {}, "PDF (*.pdf)"));
            });
    connect(remove, &QPushButton::clicked, &dialog,
            [&] { delete list->takeItem(list->currentRow()); });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("この順で結合");
    buttons->button(QDialogButtonBox::Cancel)->setText("キャンセル");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QVector<PDFDocument> sources;
    for (int i = 0; i < list->count(); ++i)
    {
        Document source;
        source.open(list->item(i)->data(Qt::UserRole).toString());
        source.editable();
        sources.append(source.pdf());
    }
    auto candidate = mergeDocuments(sources);
    auto window = new Window;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->doc.history = {std::move(candidate)};
    window->doc.saved = -1;
    ++window->doc.revision;
    window->refresh(true);
    window->show();
    window->canvas->goToPage(0);
}
} // namespace tatsu
