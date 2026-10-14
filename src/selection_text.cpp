#include "selection_text.h"
#include <QTextBoundaryFinder>
#include <algorithm>
#include <limits>

namespace tatsu
{
namespace
{
std::shared_ptr<SelectionPage> readPage(PDFDocument document, int number)
{
    auto result = std::make_shared<SelectionPage>();
    const auto crop = document.getCatalog()->getPage(number)->getCropBox();
    const auto layout = textLayout(document, number);
    for (const auto& flow :
         PDFTextFlow::createTextFlows(layout, PDFTextFlow::AddLineBreaks, number))
    {
        const auto text = flow.getText();
        const auto boxes = flow.getBoundingBoxes();
        for (qsizetype i = 0; i < text.size() && i < qsizetype(boxes.size()); ++i)
        {
            if (text[i] == QChar('\r'))
                continue;
            const auto box = boxes[i].intersected(crop);
            if (!box.isValid() && (!text[i].isSpace() || boxes[i].isValid()))
                continue;
            result->text.append(text[i]);
            result->boxes.append(box);
        }
    }
    while (result->text.endsWith('\n'))
    {
        result->text.chop(1);
        result->boxes.removeLast();
    }
    SelectionPage::Line line;
    for (qsizetype i = 0; i <= result->text.size(); ++i)
    {
        if (i == result->text.size() || result->text[i] == '\n')
        {
            line.end = i;
            if (line.bounds.isValid())
            {
                QPointF first, last;
                bool found = false;
                for (qsizetype j = line.first; j < line.end; ++j)
                    if (result->boxes[j].isValid())
                    {
                        if (!found)
                            first = result->boxes[j].center();
                        last = result->boxes[j].center();
                        found = true;
                    }
                const auto direction = last - first;
                line.vertical = qAbs(direction.y()) > qAbs(direction.x());
                line.decreasing = direction.y() < 0;
                // PDF text often encodes a space as a gap, with no glyph box.
                // Give those logical characters a caret interval on this line.
                for (qsizetype j = line.first + 1; j + 1 < line.end; ++j)
                    if (result->text[j].isSpace() && !result->boxes[j].isValid() &&
                        result->boxes[j - 1].isValid() && result->boxes[j + 1].isValid())
                    {
                        const auto left = result->boxes[j - 1].right();
                        const auto right = result->boxes[j + 1].left();
                        if (!line.vertical && right > left)
                            result->boxes[j] =
                                QRectF(left, line.bounds.top(), right - left, line.bounds.height())
                                    .intersected(crop);
                    }
                result->lines.append(line);
            }
            line = {i + 1, i + 1, {}};
        }
        else if (result->boxes[i].isValid())
            line.bounds = line.bounds.united(result->boxes[i]);
    }
    result->indexBoundaries();
    return result;
}
} // namespace
void SelectionPage::indexBoundaries()
{
    graphemes.clear();
    words.clear();
    QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
    for (qsizetype at = 0; at >= 0; at = boundary.toNextBoundary())
        graphemes.append(at);
    boundary = QTextBoundaryFinder(QTextBoundaryFinder::Word, text);
    for (qsizetype at = 0; at >= 0; at = boundary.toNextBoundary())
        if (std::binary_search(graphemes.cbegin(), graphemes.cend(), at))
            words.append(at);
    // A run of whitespace must not make a clicked word include a line break.
    for (qsizetype i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
        {
            words.append(i);
            words.append(i + 1);
        }
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
}
qsizetype SelectionPage::caret(QPointF point) const
{
    auto closest =
        std::make_pair(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    qsizetype result = 0;
    for (const auto& line : lines)
    {
        const auto& box = line.bounds;
        const auto dx = std::max({box.left() - point.x(), point.x() - box.right(), 0.0});
        const auto dy = std::max({box.top() - point.y(), point.y() - box.bottom(), 0.0});
        const auto distance = line.vertical ? std::make_pair(dx, dy) : std::make_pair(dy, dx);
        if (distance < closest)
        {
            closest = distance;
            result = line.end;
            for (qsizetype i = line.first; i < line.end; ++i)
                if (boxes[i].isValid() &&
                    (line.vertical ? (line.decreasing ? point.y() > boxes[i].center().y()
                                                      : point.y() < boxes[i].center().y())
                                   : point.x() < boxes[i].center().x()))
                {
                    result = i;
                    break;
                }
        }
    }
    const auto next = std::upper_bound(graphemes.cbegin(), graphemes.cend(), result);
    if (next != graphemes.cbegin())
        result = *std::prev(next);
    return result;
}
qsizetype SelectionPage::characterAt(QPointF point, bool nearest) const
{
    const Line* closest = nullptr;
    auto distance =
        std::make_pair(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    const auto measure = [point](QRectF box, bool vertical)
    {
        const auto dx = std::max({box.left() - point.x(), point.x() - box.right(), 0.0});
        const auto dy = std::max({box.top() - point.y(), point.y() - box.bottom(), 0.0});
        return vertical ? std::make_pair(dx, dy) : std::make_pair(dy, dx);
    };
    for (const auto& line : lines)
    {
        if (!nearest && !line.bounds.contains(point))
            continue;
        if (const auto candidate = measure(line.bounds, line.vertical); candidate < distance)
        {
            closest = &line;
            distance = candidate;
        }
    }
    if (!closest)
        return -1;
    qsizetype result = -1;
    distance = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    for (qsizetype i = closest->first; i < closest->end; ++i)
        if (boxes[i].isValid() && (nearest || boxes[i].contains(point)))
            if (const auto candidate = measure(boxes[i], closest->vertical); candidate < distance)
            {
                result = i;
                distance = candidate;
            }
    return result;
}
bool SelectionPage::contains(QPointF point) const
{
    return characterAt(point, false) >= 0;
}
QPair<qsizetype, qsizetype> SelectionPage::wordAt(QPointF point, bool nearest) const
{
    const auto index = characterAt(point, nearest);
    const auto next = std::upper_bound(words.cbegin(), words.cend(), index);
    if (index < 0 || next == words.cbegin() || next == words.cend())
        return {-1, -1};
    return {*std::prev(next), *next};
}
bool SelectionPage::hasGlyphs() const
{
    return std::any_of(boxes.begin(), boxes.end(), [](const QRectF& box) { return box.isValid(); });
}
qint64 SelectionPage::bytes() const
{
    return sizeof(SelectionPage) + text.capacity() * sizeof(QChar) +
           boxes.capacity() * sizeof(QRectF) + lines.capacity() * sizeof(Line) +
           (graphemes.capacity() + words.capacity()) * sizeof(qsizetype);
}
SelectionTextCache::SelectionTextCache(QObject* parent) : QObject(parent), executor(new QObject)
{
    executor->moveToThread(&thread);
    connect(&thread, &QThread::finished, executor, &QObject::deleteLater);
    thread.start(QThread::LowPriority);
}
SelectionTextCache::~SelectionTextCache()
{
    thread.quit();
    thread.wait();
}
void SelectionTextCache::setDocument(const PDFDocument* document, quint64 revision)
{
    if (version == revision)
        return;
    version = revision;
    ++generation;
    snapshot = document ? std::make_shared<PDFDocument>(*document) : nullptr;
    cache.clear();
    failures.clear();
    wanted.clear();
    extracted = 0;
}
void SelectionTextCache::setWanted(const QVector<int>& pages)
{
    wanted.clear();
    if (!snapshot)
        return;
    const int count = int(snapshot->getCatalog()->getPageCount());
    for (const int page : pages)
        if (page >= 0 && page < count && !wanted.contains(page))
        {
            wanted.append(page);
            if (auto it = cache.find(page); it != cache.end())
                it->used = ++clock;
        }
    prune();
    launch();
}
std::shared_ptr<const SelectionPage> SelectionTextCache::get(int page) const
{
    const auto it = cache.constFind(page);
    return it == cache.cend() ? nullptr : it->page;
}
QString SelectionTextCache::error(int page) const
{
    return failures.value(page);
}
qint64 SelectionTextCache::retainedBytes() const
{
    qint64 result = 0;
    for (const auto& entry : cache)
        result += entry.page->bytes();
    return result;
}
void SelectionTextCache::prune()
{
    qint64 size = retainedBytes();
    while (size > 16 * 1024 * 1024)
    {
        auto oldest = cache.end();
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (!wanted.contains(it.key()) && (oldest == cache.end() || it->used < oldest->used))
                oldest = it;
        if (oldest == cache.end())
            break;
        size -= oldest->page->bytes();
        cache.erase(oldest);
    }
}
void SelectionTextCache::launch()
{
    if (working || !snapshot)
        return;
    const auto next = std::find_if(wanted.begin(), wanted.end(), [this](int page)
                                   { return !cache.contains(page) && !failures.contains(page); });
    if (next == wanted.end())
        return;
    working = true;
    QMetaObject::invokeMethod(
        executor,
        [this, document = snapshot, page = *next, id = generation]
        {
            std::shared_ptr<SelectionPage> value;
            QString error;
            try
            {
                value = readPage(*document, page);
            }
            catch (const std::exception& failure)
            {
                error = QString::fromUtf8(failure.what());
            }
            catch (...)
            {
                error = "文字の解析に失敗しました";
            }
            QMetaObject::invokeMethod(
                this,
                [this, page, id, value, error]
                {
                    working = false;
                    if (id == generation)
                    {
                        if (value)
                            cache.insert(page, {value, ++clock});
                        else
                            failures.insert(page, error);
                        ++extracted;
                        prune();
                        emit pageReady(page);
                    }
                    launch();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
} // namespace tatsu
