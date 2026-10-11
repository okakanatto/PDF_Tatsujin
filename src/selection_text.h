#pragma once
#include "document.h"
#include <QThread>
#include <limits>
#include <memory>

namespace tatsu
{
struct SelectionPage
{
    struct Line
    {
        qsizetype first = 0, end = 0;
        QRectF bounds;
        bool vertical = false, decreasing = false;
    };
    QString text;
    QVector<QRectF> boxes;
    QVector<Line> lines;
    QVector<qsizetype> graphemes, words;
    void indexBoundaries();
    qsizetype caret(QPointF point) const;
    bool contains(QPointF point) const;
    QPair<qsizetype, qsizetype> wordAt(QPointF point, bool nearest = false) const;
    bool hasGlyphs() const;
    qint64 bytes() const;

private:
    qsizetype characterAt(QPointF point, bool nearest) const;
};

// Page extraction is serialized on a worker. The GUI only reads immutable values.
class SelectionTextCache final : public QObject
{
    Q_OBJECT
public:
    explicit SelectionTextCache(QObject* parent = nullptr);
    ~SelectionTextCache() override;
    void setDocument(const PDFDocument* document, quint64 revision);
    void setWanted(const QVector<int>& pages);
    std::shared_ptr<const SelectionPage> get(int page) const;
    QString error(int page) const;
    qint64 retainedBytes() const;
    int extractedPages() const
    {
        return extracted;
    }
signals:
    void pageReady(int page);

private:
    void launch();
    void prune();
    struct Entry
    {
        std::shared_ptr<const SelectionPage> page;
        quint64 used;
    };
    QThread thread;
    QObject* executor;
    std::shared_ptr<const PDFDocument> snapshot;
    QMap<int, Entry> cache;
    QHash<int, QString> failures;
    QVector<int> wanted;
    quint64 version = std::numeric_limits<quint64>::max(), generation = 0, clock = 0;
    int extracted = 0;
    bool working = false;
};
} // namespace tatsu
