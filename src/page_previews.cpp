#include "page_previews.h"
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <algorithm>

namespace tatsu
{
namespace
{
class PreviewDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return {160, 164};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(selected ? QColor("#83afea") : Qt::transparent);
        painter->setBrush(selected ? QColor("#e2ecfa") : Qt::transparent);
        painter->drawRoundedRect(option.rect.adjusted(1, 1, -1, -1), 5, 5);
        const auto image = qvariant_cast<QImage>(index.data(PagePreviews::ImageRole));
        const QRectF area(option.rect.center().x() - 48, option.rect.top() + 8, 96, 128);
        QRectF paper = area;
        if (!image.isNull())
        {
            auto size = image.deviceIndependentSize();
            size.scale(area.size(), Qt::KeepAspectRatio);
            paper = QRectF(area.center() - QPointF(size.width() / 2, size.height() / 2), size);
        }
        painter->fillRect(paper.translated(2, 2), QColor(0, 0, 0, 20));
        painter->fillRect(paper, Qt::white);
        if (!image.isNull())
            painter->drawImage(paper, image);
        else
        {
            painter->setPen(QColor("#68778c"));
            painter->drawText(paper, Qt::AlignCenter,
                              index.data(PagePreviews::StatusRole).toString());
        }
        painter->setBrush(Qt::NoBrush);
        painter->setPen(selected ? QColor("#377ccf") : QColor("#c1c9d3"));
        painter->drawRect(paper);
        painter->setPen(selected ? QColor("#154a88") : QColor("#202b3c"));
        const QRect caption = option.rect.adjusted(8, 140, -8, -4);
        painter->drawText(caption, Qt::AlignCenter,
                          option.fontMetrics.elidedText(index.data().toString(), Qt::ElideMiddle,
                                                        caption.width()));
        if (option.state.testFlag(QStyle::State_HasFocus))
        {
            painter->setPen(QPen(QColor("#377ccf"), 1, Qt::DotLine));
            painter->drawRoundedRect(option.rect.adjusted(3, 3, -3, -3), 4, 4);
        }
        painter->restore();
    }
};
} // namespace
PagePreviews::PagePreviews(QWidget* parent) : QListWidget(parent), executor(new QObject)
{
    setItemDelegate(new PreviewDelegate(this));
    setUniformItemSizes(true);
    setSpacing(6);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setAccessibleName("ページ一覧。縮小画像とページ番号、選択すると本文へ移動");
    requests.setSingleShot(true);
    connect(&requests, &QTimer::timeout, this, &PagePreviews::updateWanted);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { request(); });
    connect(this, &QListWidget::currentRowChanged, this, [this] { request(); });
    executor->moveToThread(&thread);
    connect(&thread, &QThread::finished, executor, &QObject::deleteLater);
    thread.start(QThread::LowPriority);
}
PagePreviews::~PagePreviews()
{
    thread.quit();
    thread.wait();
}
void PagePreviews::setDocument(const PDFDocument* document, quint64 version)
{
    const auto ratio = devicePixelRatioF();
    if (revision != version || pixelRatio != ratio)
    {
        revision = version;
        pixelRatio = ratio;
        ++generation;
        snapshot = document ? std::make_shared<PDFDocument>(*document) : nullptr;
        cache.clear();
        failures.clear();
        wanted.clear();
        rendered = 0;
        for (int i = 0; i < count(); ++i)
        {
            item(i)->setData(ImageRole, {});
            item(i)->setData(StatusRole, "描画中…");
        }
    }
    request();
}
QImage PagePreviews::preview(int page) const
{
    return cache.value(page).image;
}
qint64 PagePreviews::cacheBytes() const
{
    qint64 bytes = 0;
    for (const auto& entry : cache)
        bytes += entry.image.sizeInBytes();
    return bytes;
}
void PagePreviews::request()
{
    if (!requests.isActive())
        requests.start(0);
}
void PagePreviews::resizeEvent(QResizeEvent* event)
{
    QListWidget::resizeEvent(event);
    request();
}
void PagePreviews::showEvent(QShowEvent* event)
{
    QListWidget::showEvent(event);
    request();
}
void PagePreviews::hideEvent(QHideEvent* event)
{
    wanted.clear();
    requests.stop();
    QListWidget::hideEvent(event);
}
void PagePreviews::updateWanted()
{
    wanted.clear();
    if (!snapshot || !isVisible())
        return;
    if (pixelRatio != devicePixelRatioF())
        setDocument(snapshot.get(), revision);
    const int pages = int(snapshot->getCatalog()->getPageCount());
    // Uniform rows are ordered vertically. Locate the first visible row without
    // visiting every page each time the user scrolls a long document's list.
    int first = 0, end = qMin(count(), pages);
    while (first < end)
    {
        const int middle = first + (end - first) / 2;
        if (visualItemRect(item(middle)).bottom() < 0)
            first = middle + 1;
        else
            end = middle;
    }
    for (int i = first; i < qMin(count(), pages); ++i)
    {
        const auto rect = visualItemRect(item(i));
        if (rect.top() >= viewport()->height())
            break;
        if (rect.intersects(viewport()->rect()))
            wanted.append(i);
        item(i)->setToolTip(Qt::convertFromPlainText(item(i)->text()));
    }
    if (currentRow() >= 0 && currentRow() < pages && !wanted.contains(currentRow()))
        wanted.append(currentRow());
    for (const auto page : wanted)
        if (auto it = cache.find(page); it != cache.end())
        {
            it->used = ++clock;
            item(page)->setData(ImageRole, it->image);
        }
    prune();
    launch();
}
void PagePreviews::prune()
{
    while (cacheBytes() > 8 * 1024 * 1024)
    {
        auto oldest = cache.end();
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (!wanted.contains(it.key()) && (oldest == cache.end() || it->used < oldest->used))
                oldest = it;
        if (oldest == cache.end())
        {
            oldest = std::min_element(cache.begin(), cache.end(), [](const Entry& a, const Entry& b)
                                      { return a.used < b.used; });
            // Do not repeatedly render an evicted visible row in the same cycle.
            wanted.removeAll(oldest.key());
        }
        if (oldest.key() < count())
        {
            item(oldest.key())->setData(ImageRole, {});
            item(oldest.key())->setData(StatusRole, "未描画");
        }
        cache.erase(oldest);
    }
}
void PagePreviews::launch()
{
    if (working || !snapshot)
        return;
    const auto next = std::find_if(wanted.cbegin(), wanted.cend(), [this](int page)
                                   { return !cache.contains(page) && !failures.contains(page); });
    if (next == wanted.cend())
        return;
    working = true;
    QMetaObject::invokeMethod(
        executor,
        [this, document = snapshot, page = *next, id = generation, ratio = pixelRatio]
        {
            QImage image;
            QString error;
            try
            {
                const auto size = pageSize(document->getCatalog()->getPage(page));
                const auto scale = qMin(96.0 / size.width(), 128.0 / size.height()) * ratio;
                image = renderPage(*document, page, scale);
                image.setDevicePixelRatio(ratio);
            }
            catch (const std::exception&)
            {
                error = "描画できません";
            }
            catch (...)
            {
                error = "描画できません";
            }
            QMetaObject::invokeMethod(
                this,
                [this, page, id, image, error]
                {
                    working = false;
                    if (id == generation)
                    {
                        if (!image.isNull())
                        {
                            cache.insert(page, {image, ++clock});
                            if (page < count())
                                item(page)->setData(ImageRole, image);
                        }
                        else
                        {
                            failures.insert(page, error);
                            if (page < count())
                                item(page)->setData(StatusRole, error);
                        }
                        ++rendered;
                        prune();
                    }
                    launch();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
} // namespace tatsu
