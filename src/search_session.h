#pragma once
#include "document.h"
#include <QAbstractListModel>
#include <QThread>
#include <atomic>
#include <memory>
#include <optional>

namespace tatsu
{
struct SearchMatch
{
    quint64 id = 0;
    int page = 0;
    QString excerpt;
    QVector<QRectF> boxes;
    QRectF bounds;
};

// Owns an immutable input and at most one worker plus one replacement request.
// Rows are in document order even though the current page is searched first.
class SearchSession final : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit SearchSession(QObject* parent = nullptr);
    ~SearchSession() override;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void start(PDFDocument document, QString query, int firstPage);
    void clear();
    const QVector<SearchMatch>& matches() const
    {
        return rows;
    }
    int indexOf(quint64 id) const;
    bool complete() const
    {
        return finished;
    }
    bool changing() const
    {
        return inserting;
    }
    int processedPages() const
    {
        return processed;
    }
    int totalPages() const
    {
        return total;
    }
    int textlessPages() const
    {
        return textless;
    }
    QStringList errors() const
    {
        return failures;
    }
signals:
    void updated();

private:
    struct Request
    {
        PDFDocument document;
        QString query;
        int firstPage;
        quint64 generation;
    };
    void launch();
    QVector<SearchMatch> rows;
    QStringList failures;
    std::optional<Request> pending;
    std::shared_ptr<std::atomic_bool> cancelled;
    QThread* worker = nullptr;
    quint64 generation = 0;
    int processed = 0, total = 0, textless = 0;
    bool finished = true, inserting = false;
};
} // namespace tatsu
