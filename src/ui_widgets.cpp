#include "ui_widgets.h"

namespace tatsu
{
ElidedLabel::ElidedLabel(const QString& text, QWidget* parent) : QLabel(text, parent)
{
    setTextFormat(Qt::PlainText);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}
void ElidedLabel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QStyleOption option;
    option.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
    painter.setPen(
        palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
    const auto area = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    painter.drawText(area, QStyle::visualAlignment(layoutDirection(), alignment()),
                     fontMetrics().elidedText(text(), Qt::ElideRight, area.width()));
}
bool ElidedLabel::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip)
    {
        const auto help = static_cast<QHelpEvent*>(event);
        QToolTip::showText(help->globalPos(), text(), this);
        return true;
    }
    return QLabel::event(event);
}
QScrollArea* scrollableSettings(QWidget* content)
{
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    QObject::connect(qApp, &QApplication::focusChanged, scroll,
                     [scroll, content](QWidget*, QWidget* focused)
                     {
                         if (!focused || !content->isAncestorOf(focused))
                             return;
                         // QAction/buddy focus changes need the same reveal as Tab.
                         // Wait for the panel's layout and discard obsolete requests.
                         const QPointer<QWidget> target(focused);
                         QTimer::singleShot(0, scroll,
                                            [scroll, target]
                                            {
                                                if (target && target->hasFocus())
                                                    scroll->ensureWidgetVisible(target, 16, 16);
                                            });
                     });
    return scroll;
}
} // namespace tatsu
