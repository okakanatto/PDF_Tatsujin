#pragma once
#include "view_state.h"
#include <QString>
#include <QVector>
#include <optional>

namespace tatsu
{
// Reading history is independent of PDF edit Undo and of the window/widgets.
class ViewHistory
{
public:
    enum class Direction
    {
        Back,
        Forward
    };
    struct Entry
    {
        ViewState view;
        QString query;
        quint64 revision = 0;

        quint64 activeSearchFor(const QString& currentQuery, quint64 currentRevision) const;
    };
    static constexpr int limit = 200;
    void clear();
    void remember(Entry entry);
    bool canMove(Direction direction) const;
    std::optional<Entry> move(Direction direction, Entry current);

private:
    QVector<Entry> back, forward;
    static void append(QVector<Entry>& entries, Entry entry);
};
} // namespace tatsu
