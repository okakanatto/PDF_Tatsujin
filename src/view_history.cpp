#include "view_history.h"
#include <utility>

namespace tatsu
{
quint64 ViewHistory::Entry::activeSearchFor(const QString& currentQuery,
                                            quint64 currentRevision) const
{
    return query == currentQuery && revision == currentRevision ? view.activeSearch : 0;
}
void ViewHistory::clear()
{
    back.clear();
    forward.clear();
}
void ViewHistory::append(QVector<Entry>& entries, Entry entry)
{
    entries.append(std::move(entry));
    if (entries.size() > limit)
        entries.removeFirst();
}
void ViewHistory::remember(Entry entry)
{
    if (entry.view.anchor.page < 0)
        return;
    if (back.isEmpty() || !back.back().view.samePosition(entry.view) ||
        back.back().view.activeSearch != entry.view.activeSearch ||
        back.back().query != entry.query || back.back().revision != entry.revision)
        append(back, std::move(entry));
    forward.clear();
}
bool ViewHistory::canMove(Direction direction) const
{
    return !(direction == Direction::Forward ? forward : back).isEmpty();
}
std::optional<ViewHistory::Entry> ViewHistory::move(Direction direction, Entry current)
{
    auto& source = direction == Direction::Forward ? forward : back;
    auto& target = direction == Direction::Forward ? back : forward;
    if (source.isEmpty() || current.view.anchor.page < 0)
        return std::nullopt;
    append(target, std::move(current));
    return source.takeLast();
}
} // namespace tatsu
