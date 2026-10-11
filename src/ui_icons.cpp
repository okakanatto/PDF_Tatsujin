#include "ui_icons.h"
#include <QApplication>
#include <QHash>
#include <QPointer>
#include <QThread>

namespace tatsu
{
QIcon uiIcon(QStyle::StandardPixmap icon)
{
    Q_ASSERT(qApp && QThread::currentThread() == qApp->thread());
    struct Cache
    {
        QPointer<QStyle> style;
        qint64 palette = 0;
        QHash<int, QIcon> icons;
    };
    static Cache cache;
    auto style = qApp->style();
    const auto palette = qApp->palette().cacheKey();
    if (cache.style != style || cache.palette != palette)
    {
        cache.icons.clear();
        cache.style = style;
        cache.palette = palette;
    }
    auto found = cache.icons.constFind(int(icon));
    if (found == cache.icons.cend())
        found = cache.icons.insert(int(icon), style->standardIcon(icon));
    return found.value();
}
} // namespace tatsu
