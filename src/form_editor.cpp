#include "form_editor.h"
#include "canvas.h"
#include <QInputMethodEvent>

namespace tatsu
{
FormEditor::FormEditor(Document* doc, Canvas* view) : QObject(view), document(doc), canvas(view) {}
void FormEditor::refresh()
{
    if (revision == document->revision)
        return;
    cancel();
    fields = document->loaded() ? formFields(document->pdf()) : QVector<FormField>();
    revision = document->revision;
}
std::optional<Qt::CursorShape> FormEditor::cursorAt(int page, QPointF point) const
{
    for (const auto& item : fields)
        if (item.page == page && item.rectangle.contains(point))
            return item.kind == FormKind::Text || item.kind == FormKind::Multiline
                       ? Qt::IBeamCursor
                       : Qt::PointingHandCursor;
    return {};
}
bool FormEditor::press(int page, QPointF point)
{
    refresh();
    for (const auto& item : fields)
        if (item.page == page && item.rectangle.contains(point))
        {
            const auto selected = item;
            if (selected.readOnly || selected.kind == FormKind::Unsupported || document->busy ||
                !document->readOnly.isEmpty())
            {
                QToolTip::showText(canvas->mapToGlobal(QPoint()),
                                   selected.notice.isEmpty() ? "このフォームは読み取り専用です。"
                                                             : selected.notice);
                return true;
            }
            if (editor && field.widget == selected.widget)
            {
                editor->setFocus();
                return true;
            }
            finish();
            if (selected.kind == FormKind::Checkbox || selected.kind == FormKind::Radio)
            {
                const QString value =
                    selected.kind == FormKind::Radio || selected.values.value(0) != selected.onState
                        ? selected.onState
                        : "Off";
                putFormValue(*document, selected.widget, {value});
                if (changed)
                    changed();
            }
            else
                open(selected);
            return true;
        }
    finish();
    return false;
}
void FormEditor::open(const FormField& selected)
{
    field = selected;
    editRevision = document->revision;
    preedit = false;
    if (field.kind == FormKind::Text)
    {
        auto input = new QLineEdit(canvas->viewport());
        input->setText(field.values.value(0));
        editor = input;
        connect(input, &QLineEdit::textChanged, this, [this] { updateDraft(); });
    }
    else if (field.kind == FormKind::Multiline)
    {
        auto input = new QPlainTextEdit(canvas->viewport());
        input->setPlainText(field.values.value(0));
        editor = input;
        connect(input, &QPlainTextEdit::textChanged, this, [this] { updateDraft(); });
    }
    else if (field.kind == FormKind::Combo)
    {
        auto input = new QComboBox(canvas->viewport());
        input->setEditable(field.editableChoice);
        for (int i = 0; i < field.labels.size(); ++i)
            input->addItem(field.labels[i], field.exports[i]);
        input->setCurrentIndex(field.exports.indexOf(field.values.value(0)));
        if (field.editableChoice)
            input->setEditText(field.values.value(0));
        editor = input;
        connect(input, &QComboBox::currentIndexChanged, this, [this] { updateDraft(); });
        connect(input, &QComboBox::editTextChanged, this, [this] { updateDraft(); });
        if (input->lineEdit())
            input->lineEdit()->installEventFilter(this);
    }
    else if (field.kind == FormKind::List)
    {
        auto input = new QListWidget(canvas->viewport());
        input->setSelectionMode(field.multiple ? QAbstractItemView::ExtendedSelection
                                               : QAbstractItemView::SingleSelection);
        for (int i = 0; i < field.labels.size(); ++i)
        {
            input->addItem(field.labels[i]);
            input->item(i)->setSelected(field.values.contains(field.exports[i]));
        }
        const int current = field.exports.indexOf(field.values.value(0));
        if (current >= 0)
            input->setCurrentRow(current, QItemSelectionModel::NoUpdate);
        editor = input;
        connect(input, &QListWidget::itemSelectionChanged, this, [this] { updateDraft(); });
    }
    if (!editor)
        return;
    editor->setObjectName("activeFormEditor");
    editor->setAccessibleName(field.name.isEmpty() ? "PDFフォーム入力" : field.name);
    editor->setStyleSheet("padding: 1px; border: 2px solid #377ccf; background: white;");
    editor->installEventFilter(this);
    reposition();
    editor->show();
    editor->setFocus(Qt::MouseFocusReason);
    updateDraft();
}
QStringList FormEditor::values() const
{
    if (auto input = qobject_cast<QAbstractButton*>(editor.data()))
    {
        if (field.kind == FormKind::Radio && !input->isChecked() &&
            field.values.value(0) != field.onState)
            return field.values;
        return {input->isChecked() ? field.onState : QString("Off")};
    }
    if (auto input = qobject_cast<QLineEdit*>(editor.data()))
        return {input->text()};
    if (auto input = qobject_cast<QPlainTextEdit*>(editor.data()))
        return {input->toPlainText()};
    if (auto input = qobject_cast<QComboBox*>(editor.data()))
    {
        if (field.editableChoice && input->currentText() != input->itemText(input->currentIndex()))
            return {input->currentText()};
        return {input->currentData().toString()};
    }
    QStringList result;
    if (auto input = qobject_cast<QListWidget*>(editor.data()))
        for (int i = 0; i < input->count(); ++i)
            if (input->item(i)->isSelected())
                result.append(field.exports[i]);
    return result;
}
void FormEditor::updateDraft()
{
    document->pendingInput = editor && (preedit || values() != field.values);
    if (draftChanged)
        draftChanged();
}
void FormEditor::finish()
{
    if (!editor || finishing)
        return;
    if (editRevision != document->revision)
    {
        cancel();
        return;
    }
    QGuiApplication::inputMethod()->commit();
    if (preedit)
        fail("日本語入力を確定してから操作してください。入力内容は保持しています。");
    finishing = true;
    try
    {
        const auto input = values();
        const auto before = document->revision;
        if (input != field.values)
            putFormValue(*document, field.widget, input);
        cancel();
        finishing = false;
        if (document->revision != before && changed)
            changed();
    }
    catch (...)
    {
        finishing = false;
        updateDraft();
        throw;
    }
}
void FormEditor::cancel()
{
    auto old = editor;
    const bool hadDraft = document->pendingInput;
    editor = nullptr;
    preedit = false;
    document->pendingInput = false;
    if (old)
    {
        old->setObjectName("retiredFormEditor");
        old->hide();
        old->deleteLater();
    }
    if ((old || hadDraft) && draftChanged)
        draftChanged();
}
void FormEditor::reposition()
{
    if (!editor || !document->loaded())
        return;
    if (!canvas->visiblePages().contains(field.page))
    {
        editor->hide();
        return;
    }
    editor->show();
    const QRectF rectangle(canvas->pdfToViewport(field.page, field.rectangle.topLeft()),
                           canvas->pdfToViewport(field.page, field.rectangle.bottomRight()));
    editor->setGeometry(rectangle.normalized().toAlignedRect());
    auto font = canvas->font();
    font.setPixelSize(qMax(10, qRound(qMin(18.0, rectangle.normalized().height() * .55))));
    editor->setFont(font);
}
bool FormEditor::focusNext(bool next)
{
    refresh();
    auto current = editor ? field.widget : PDFObjectReference();
    finish();
    refresh();
    int index = next ? -1 : fields.size();
    if (!current.isValid())
        for (int i = 0; i < fields.size(); ++i)
            if (fields[i].page == canvas->page && !fields[i].readOnly &&
                fields[i].kind != FormKind::Unsupported)
            {
                index = next ? i - 1 : i + 1;
                break;
            }
    for (int i = 0; i < fields.size(); ++i)
        if (fields[i].widget == current)
            index = i;
    for (int offset = 1; offset <= fields.size(); ++offset)
    {
        int candidate = (index + (next ? offset : -offset) + fields.size() * 2) % fields.size();
        const auto target = fields[candidate];
        if (target.readOnly || target.kind == FormKind::Unsupported)
            continue;
        canvas->restoreAnchor({target.page, target.rectangle.center(), {.5, .35}});
        if (target.kind == FormKind::Checkbox || target.kind == FormKind::Radio)
        {
            QAbstractButton* input =
                target.kind == FormKind::Radio
                    ? static_cast<QAbstractButton*>(new QRadioButton(canvas->viewport()))
                    : static_cast<QAbstractButton*>(new QCheckBox(canvas->viewport()));
            input->setAutoExclusive(false);
            field = target;
            editRevision = document->revision;
            input->setChecked(target.values.value(0) == target.onState);
            editor = input;
            input->setObjectName("activeFormEditor");
            input->setAccessibleName(target.name);
            input->installEventFilter(this);
            connect(input, &QAbstractButton::toggled, this,
                    [this, input]
                    {
                        if (field.kind == FormKind::Radio && field.noToggleOff &&
                            !input->isChecked() && field.values.value(0) == field.onState)
                        {
                            QSignalBlocker block(input);
                            input->setChecked(true);
                            return;
                        }
                        try
                        {
                            putFormValue(*document, field.widget,
                                         {input->isChecked() ? field.onState : "Off"});
                            cancel();
                            if (changed)
                                changed();
                        }
                        catch (const std::exception& error)
                        {
                            showError(error);
                        }
                    });
            reposition();
            input->show();
            input->setFocus(Qt::TabFocusReason);
        }
        else
            open(target);
        return true;
    }
    return false;
}
void FormEditor::showError(const std::exception& error)
{
    QMessageBox::warning(canvas, "フォーム入力を確定できません", QString::fromUtf8(error.what()));
    if (editor)
        editor->setFocus();
}
bool FormEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (!editor)
        return false;
    auto target = qobject_cast<QWidget*>(watched);
    if (target != editor && (!target || !editor->isAncestorOf(target)))
        return false;
    if (event->type() == QEvent::ShortcutOverride &&
        (qobject_cast<QLineEdit*>(target) || qobject_cast<QPlainTextEdit*>(target)))
    {
        auto key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo))
        {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::InputMethod)
    {
        preedit = !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
        QTimer::singleShot(0, this, [this] { updateDraft(); });
    }
    if (event->type() == QEvent::KeyPress)
    {
        auto key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && !preedit)
        {
            cancel();
            canvas->setFocus();
            return true;
        }
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) && !preedit)
        {
            try
            {
                focusNext(key->key() == Qt::Key_Tab &&
                          !key->modifiers().testFlag(Qt::ShiftModifier));
            }
            catch (const std::exception& error)
            {
                showError(error);
            }
            return true;
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !preedit &&
            (field.kind != FormKind::Multiline || key->modifiers().testFlag(Qt::ControlModifier)))
        {
            try
            {
                finish();
                canvas->setFocus();
            }
            catch (const std::exception& error)
            {
                showError(error);
            }
            return true;
        }
    }
    if (event->type() == QEvent::FocusOut)
    {
        QTimer::singleShot(0, this,
                           [this]
                           {
                               if (!editor || finishing)
                                   return;
                               const auto focus = QApplication::focusWidget();
                               if (focus == editor || (focus && editor->isAncestorOf(focus)))
                                   return;
                               if (qobject_cast<QComboBox*>(editor.data()) &&
                                   QApplication::activePopupWidget())
                                   return;
                               try
                               {
                                   finish();
                               }
                               catch (const std::exception& error)
                               {
                                   showError(error);
                               }
                           });
    }
    return false;
}
} // namespace tatsu
