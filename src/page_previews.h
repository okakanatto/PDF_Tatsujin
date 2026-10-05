#pragma once
#include "document.h"
#include <QListWidget>
#include <QThread>
#include <QTimer>
#include <limits>
#include <memory>

namespace tatsu
{
class PagePreviews final : public QListWidget
{
    Q_OBJECT
public:
    enum
    {
        ImageRole = Qt::UserRole,
        StatusRole
    };
    explicit PagePreviews(QWidget* parent = nullptr);
    ~PagePreviews() override;
    void setDocument(const PDFDocument* document, quint64 revision);
    QImage preview(int page) const;
    int renderedPages() const
    {
        return rendered;
    }
    qint64 cacheBytes() const;

protected:
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    void request();
    void updateWanted();
    void launch();
    void prune();
    struct Entry
    {
        QImage image;
        quint64 used;
    };
    QThread thread;
    QObject* executor;
    QTimer requests;
    std::shared_ptr<PDFDocument> snapshot;
    QMap<int, Entry> cache;
    QHash<int, QString> failures;
    QVector<int> wanted;
    quint64 revision = std::numeric_limits<quint64>::max(), generation = 0, clock = 0;
    double pixelRatio = 1;
    bool working = false;
    int rendered = 0;
};
} // namespace tatsu
