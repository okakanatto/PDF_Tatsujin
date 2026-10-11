#include "search_panel.h"

namespace tatsu
{
namespace
{
class SearchResultsView final : public QListView
{
public:
    bool pointerChangeActive() const
    {
        return pointerChange;
    }

protected:
    bool viewportEvent(QEvent* event) override
    {
        switch (event->type())
        {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        {
            QScopedValueRollback<bool> guard(pointerChange, true);
            return QListView::viewportEvent(event);
        }
        default:
            return QListView::viewportEvent(event);
        }
    }

private:
    bool pointerChange = false;
};
} // namespace
void SearchEdit::inputMethodEvent(QInputMethodEvent* event)
{
    const bool wasComposing = composing;
    composing = !event->preeditString().isEmpty();
    QLineEdit::inputMethodEvent(event);
    if (wasComposing && !composing && compositionEnded)
        compositionEnded();
}
void SearchEdit::keyPressEvent(QKeyEvent* event)
{
    if (!composing && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter))
    {
        if (submit)
            submit(event->modifiers().testFlag(Qt::ShiftModifier) ? -1 : 1);
        event->accept();
        return;
    }
    if (!composing && event->key() == Qt::Key_Escape)
    {
        if (leave)
            leave();
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

SearchPanel::SearchPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("searchPanel");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    query = new SearchEdit;
    query->setPlaceholderText("本文・OCR文字を検索");
    query->setAccessibleName("文書検索");
    query->setClearButtonEnabled(true);
    for (auto button : query->findChildren<QToolButton*>())
    {
        button->setAccessibleName("検索を消去");
        button->setToolTip("検索を消去");
    }
    layout->addWidget(query);
    auto controls = new QHBoxLayout;
    previous = new QPushButton("前の一致");
    previous->setObjectName("previousMatch");
    previous->setToolTip("前の一致（Shift+F3 / Shift+Enter）");
    following = new QPushButton("次の一致");
    following->setObjectName("nextMatch");
    following->setToolTip("次の一致（F3 / Enter）");
    controls->addWidget(previous);
    controls->addWidget(following);
    layout->addLayout(controls);
    summary = new QLabel;
    summary->setObjectName("searchSummary");
    summary->setWordWrap(true);
    summary->setTextFormat(Qt::PlainText);
    layout->addWidget(summary);
    results = new SearchSession(this);
    auto resultView = new SearchResultsView;
    list = resultView;
    list->setObjectName("searchResults");
    list->setAccessibleName("検索結果。一致ごとの抜粋とページ番号");
    list->setModel(results);
    list->setWordWrap(true);
    list->setSpacing(3);
    list->setStyleSheet(
        "QListView { background: #f4f6f9; border: 0; outline: 0; }"
        "QListView::item { padding: 10px 7px; border: 1px solid transparent; border-radius: 5px; }"
        "QListView::item:selected { background: #e2ecfa; color: #154a88; border-color: #9dbce6; }"
        "QListView::item:focus { border-color: #377ccf; }");
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(list, 1);
    hint = new QLabel;
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #596779; font-size: 12px;");
    layout->addWidget(hint);
    debounce.setSingleShot(true);
    debounce.setInterval(150);
    connect(&debounce, &QTimer::timeout, this, [this] { startSearch(); });
    connect(query, &QLineEdit::textChanged, this, [this] { schedule(); });
    query->compositionEnded = [this] { schedule(); };
    query->submit = [this](int direction) { next(direction); };
    query->leave = [this] { emit returnToDocument(); };
    connect(previous, &QPushButton::clicked, this, [this] { next(-1); });
    connect(following, &QPushButton::clicked, this, [this] { next(1); });
    connect(list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this, resultView](const QModelIndex& index)
            {
                if (!resultView->pointerChangeActive() && !results->changing() && index.isValid())
                    activate(index.row());
            });
    auto activateResult = [this](const QModelIndex& index)
    {
        if (!results->changing() && index.isValid())
            activate(index.row());
    };
    connect(list, &QListView::clicked, this, activateResult);
    connect(list, &QListView::activated, this,
            [resultView, activateResult](const QModelIndex& index)
            {
                // Some styles also activate on a mouse click; clicked already
                // handles that gesture, while Enter needs its own activation.
                if (!resultView->pointerChangeActive())
                    activateResult(index);
            });
    connect(results, &SearchSession::updated, this, &SearchPanel::updateResults);
    updateResults();
}
void SearchPanel::setDocument(const PDFDocument* document, quint64 version, int currentPage)
{
    firstPage = currentPage;
    if (version == revision)
        return;
    revision = version;
    snapshot = document ? std::optional<PDFDocument>(*document) : std::nullopt;
    debounce.stop();
    searched.clear();
    active = 0;
    pendingDirection = 0;
    results->clear();
    if (snapshot && !query->text().isEmpty() && !query->composing)
        startSearch();
}
void SearchPanel::schedule()
{
    debounce.stop();
    active = 0;
    pendingDirection = 0;
    searched.clear();
    notice.clear();
    results->clear();
    if (snapshot && !query->text().isEmpty() && !query->composing)
        debounce.start();
    updateResults();
}
void SearchPanel::startSearch(int direction)
{
    debounce.stop();
    if (!snapshot || query->text().isEmpty() || query->composing)
        return;
    active = 0;
    searched = query->text();
    pendingDirection = 0;
    results->start(*snapshot, searched, firstPage);
    pendingDirection = direction;
    updateResults();
}
void SearchPanel::next(int direction)
{
    if (query->composing)
        return;
    notice.clear();
    if (searched != query->text() || debounce.isActive())
    {
        startSearch(direction);
        return;
    }
    if (searched.isEmpty())
        return;
    pendingDirection = direction;
    updateResults();
}
void SearchPanel::restoreActive(quint64 id)
{
    const int row = results->indexOf(id);
    active = row >= 0 ? id : 0;
    pendingDirection = 0;
    notice.clear();
    {
        QSignalBlocker block(list->selectionModel());
        list->setCurrentIndex(row >= 0 ? results->index(row) : QModelIndex());
        if (row >= 0)
            list->scrollTo(results->index(row));
    }
    updateResults();
}
void SearchPanel::activate(int row)
{
    if (row < 0 || row >= results->rowCount())
        return;
    const auto match = results->matches()[row];
    active = match.id;
    pendingDirection = 0;
    {
        QSignalBlocker block(list->selectionModel());
        list->setCurrentIndex(results->index(row));
        list->scrollTo(results->index(row));
    }
    // A repeated command must also return to the occurrence after the reader
    // has scrolled away, including when the document has only one match.
    emit matchActivated(match);
    updateResults();
}
void SearchPanel::updateResults()
{
    const int count = results->rowCount();
    int row = results->indexOf(active);
    if (pendingDirection && count)
    {
        int target = row < 0 ? (pendingDirection > 0 ? 0 : count - 1) : row + pendingDirection;
        if ((target < 0 || target >= count) && results->complete())
        {
            notice = target < 0 ? "末尾へ戻りました" : "先頭へ戻りました";
            target = target < 0 ? count - 1 : 0;
        }
        if (target >= 0 && target < count)
        {
            activate(target);
            return;
        }
    }
    if (results->complete())
        pendingDirection = 0;
    previous->setEnabled(!query->text().isEmpty() && !query->composing);
    following->setEnabled(previous->isEnabled());
    if (query->text().isEmpty())
        summary->setText("本文やOCR文字を検索します");
    else if (query->composing)
        summary->setText("文字の確定を待っています");
    else if (searched != query->text())
        summary->setText("入力待ち…");
    else if (!results->complete())
        summary->setText(QString("検索中 %1 / %2ページ · %3件%4")
                             .arg(results->processedPages())
                             .arg(results->totalPages())
                             .arg(count)
                             .arg(pendingDirection ? "\n続きの検索を待っています" : ""));
    else if (!results->errors().isEmpty())
        summary->setText(QString("検索終了 · %1件\n解析できないページがあります").arg(count));
    else
        summary->setText(count ? (row < 0 ? QString("%1件").arg(count)
                                          : QString("%1 / %2件%3")
                                                .arg(row + 1)
                                                .arg(count)
                                                .arg(notice.isEmpty() ? "" : "\n" + notice))
                               : "一致する文字はありません");
    summary->setToolTip(results->errors().join("\n").toHtmlEscaped());
    hint->setText(results->textlessPages()
                      ? QString("文字情報のないページが%1ページあります。OCRで検索可能にできます。")
                            .arg(results->textlessPages())
                      : "Escで本文へ戻れます。検索を消去するには入力欄の×を押します。");
    emit presentationChanged();
}
} // namespace tatsu
