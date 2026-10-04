#include "search_session.h"
#include <algorithm>

namespace tatsu
{
namespace
{
QVector<SearchMatch> findMatches(PDFDocument& document, int page, const QString& query,
                                 const std::atomic_bool& cancelled, bool& textless)
{
    auto layout = textLayout(document, page);
    const auto crop = document.getCatalog()->getPage(page)->getCropBox();
    QVector<SearchMatch> result;
    textless = true;
    int ordinal = 0;
    for (const auto& flow : PDFTextFlow::createTextFlows(layout, PDFTextFlow::AddLineBreaks, page))
    {
        if (cancelled.load())
            return {};
        const auto text = flow.getText();
        const auto boxes = flow.getBoundingBoxes();
        for (qsizetype i = 0; i < text.size() && i < qsizetype(boxes.size()); ++i)
            if (!text[i].isSpace() && crop.intersects(boxes[i]))
                textless = false;
        qsizetype from = 0;
        while ((from = text.indexOf(query, from, Qt::CaseInsensitive)) >= 0)
        {
            if (cancelled.load())
                return {};
            SearchMatch match;
            match.id = (quint64(page) << 32) | quint32(++ordinal);
            match.page = page;
            const auto first = qMax(qsizetype(0), from - 22);
            match.excerpt = (first ? QStringLiteral("…") : QString()) +
                            text.mid(first, query.size() + 44).simplified();
            if (first + query.size() + 44 < text.size())
                match.excerpt += QStringLiteral("…");
            for (qsizetype i = from; i < from + query.size() && i < qsizetype(boxes.size()); ++i)
                if (const auto visible = boxes[i].intersected(crop); visible.isValid())
                {
                    match.boxes.append(visible);
                    match.bounds = match.bounds.united(visible);
                }
            if (!match.boxes.isEmpty())
                result.append(std::move(match));
            from += query.size();
        }
    }
    return result;
}
} // namespace

SearchSession::SearchSession(QObject* parent) : QAbstractListModel(parent) {}
SearchSession::~SearchSession()
{
    if (cancelled)
        cancelled->store(true);
    if (worker)
    {
        // The worker owns its PDF and never waits on the UI. Finish the current
        // text-layout call before destroying the context for queued callbacks.
        worker->wait();
        delete worker;
    }
}
int SearchSession::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(rows.size());
}
QVariant SearchSession::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows.size())
        return {};
    const auto& match = rows[index.row()];
    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole)
        return QString("%1ページ\n%2").arg(match.page + 1).arg(match.excerpt);
    if (role == Qt::ToolTipRole)
        return match.excerpt.toHtmlEscaped();
    return {};
}
int SearchSession::indexOf(quint64 id) const
{
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i].id == id)
            return i;
    return -1;
}
void SearchSession::clear()
{
    ++generation;
    if (cancelled)
        cancelled->store(true);
    pending.reset();
    inserting = true;
    beginResetModel();
    rows.clear();
    endResetModel();
    inserting = false;
    failures.clear();
    processed = total = textless = 0;
    finished = true;
    emit updated();
}
void SearchSession::start(PDFDocument document, QString query, int firstPage)
{
    clear();
    if (query.isEmpty())
        return;
    total = int(document.getCatalog()->getPageCount());
    finished = false;
    pending =
        Request{std::move(document), std::move(query), qBound(0, firstPage, total - 1), generation};
    emit updated();
    launch();
}
void SearchSession::launch()
{
    if (worker || !pending)
        return;
    auto request = std::move(*pending);
    pending.reset();
    cancelled = std::make_shared<std::atomic_bool>(false);
    const auto stop = cancelled;
    worker = QThread::create(
        [this, request = std::move(request), stop]() mutable
        {
            const int count = int(request.document.getCatalog()->getPageCount());
            for (int offset = 0; offset < count && !stop->load(); ++offset)
            {
                const int page = (request.firstPage + offset) % count;
                QVector<SearchMatch> matches;
                QString error;
                bool empty = false;
                try
                {
                    matches = findMatches(request.document, page, request.query, *stop, empty);
                }
                catch (const std::exception& failure)
                {
                    error = QString("%1ページ: %2")
                                .arg(page + 1)
                                .arg(QString::fromUtf8(failure.what()));
                }
                catch (...)
                {
                    error = QString("%1ページ: 文字の解析に失敗しました").arg(page + 1);
                }
                if (stop->load())
                    return;
                QMetaObject::invokeMethod(
                    this,
                    [this, id = request.generation, page, empty, error,
                     matches = std::move(matches)]() mutable
                    {
                        if (id != generation)
                            return;
                        if (!matches.isEmpty())
                        {
                            auto it = std::lower_bound(rows.begin(), rows.end(), page,
                                                       [](const SearchMatch& match, int value)
                                                       { return match.page < value; });
                            const int at = int(it - rows.begin());
                            inserting = true;
                            beginInsertRows({}, at, at + int(matches.size()) - 1);
                            rows.insert(at, matches.size(), SearchMatch());
                            std::move(matches.begin(), matches.end(), rows.begin() + at);
                            endInsertRows();
                            inserting = false;
                        }
                        if (!error.isEmpty())
                            failures.append(error);
                        textless += empty ? 1 : 0;
                        finished = ++processed == total;
                        emit updated();
                    },
                    Qt::QueuedConnection);
            }
        });
    worker->setParent(this);
    connect(worker, &QThread::finished, this,
            [this]
            {
                worker->deleteLater();
                worker = nullptr;
                launch();
            });
    worker->start(QThread::LowPriority);
}
} // namespace tatsu
