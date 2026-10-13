#include "page_control.h"

namespace tatsu
{
PageControl::PageControl(QWidget* parent) : QWidget(parent)
{
    setObjectName("pageControl");
    auto row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    number = new QLineEdit;
    number->setObjectName("pageNumber");
    number->setAccessibleName("ページ番号（先頭から数えた番号）");
    number->setAlignment(Qt::AlignRight);
    number->setFixedWidth(64);
    number->setMaxLength(12);
    number->installEventFilter(this);
    total = new QLabel;
    total->setTextFormat(Qt::PlainText);
    row->addWidget(number);
    row->addWidget(total);
    connect(number, &QLineEdit::returnPressed, this, &PageControl::validateNumber);
    connect(number, &QLineEdit::textEdited, this, [this] { setError({}); });
    setPage(0, 0);
}
void PageControl::setPage(int page, int count)
{
    current = page;
    pageCount = count;
    setEnabled(count > 0);
    total->setText(QString("/ %1").arg(count));
    if (!number->hasFocus())
        resetNumber();
}
void PageControl::focusNumber()
{
    if (pageCount <= 0)
        return;
    number->setFocus(Qt::ShortcutFocusReason);
    number->selectAll();
}
QString PageControl::validationMessage() const
{
    return error;
}
void PageControl::setError(const QString& message)
{
    const bool changed = error != message;
    error = message;
    number->setToolTip(
        error.isEmpty()
            ? "ページ番号を入力しEnterで移動（Ctrl+"
              "L）。文書内のラベルではなく、先頭から数えた番号です。Escで入力を戻します。"
            : error);
    number->setAccessibleDescription(number->toolTip());
    if (changed)
    {
        number->setProperty("invalidPage", !error.isEmpty());
        number->style()->unpolish(number);
        number->style()->polish(number);
        emit validationChanged();
    }
}
void PageControl::resetNumber()
{
    number->setText(pageCount > 0 ? QString::number(current + 1) : QString());
    setError({});
}
void PageControl::validateNumber()
{
    if (composing || pageCount <= 0)
        return;
    const auto text = number->text();
    bool converted = false;
    const auto value = text.toLongLong(&converted);
    if (!QRegularExpression("^[0-9]+$").match(text).hasMatch() || !converted || value < 1 ||
        value > pageCount)
    {
        setError(QString("1〜%1のページ番号を入力してください。Enterで移動、Escで入力を戻します。")
                     .arg(pageCount));
        return;
    }
    setError({});
    emit pageRequested(int(value - 1));
    emit returnToDocument();
}
bool PageControl::eventFilter(QObject* object, QEvent* event)
{
    if (object == number)
    {
        if (event->type() == QEvent::InputMethod)
            composing = !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
        else if (event->type() == QEvent::FocusOut)
        {
            composing = false;
            resetNumber();
        }
        else if (event->type() == QEvent::KeyPress)
        {
            auto key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape && !composing)
            {
                resetNumber();
                emit returnToDocument();
                return true;
            }
        }
    }
    return QWidget::eventFilter(object, event);
}
} // namespace tatsu
