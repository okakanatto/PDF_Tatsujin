#pragma once
#include "form_fields.h"
#include <QtWidgets>

namespace tatsu
{
class Canvas;
class FormEditor : public QObject
{
public:
    FormEditor(Document* document, Canvas* canvas);
    std::function<void()> changed, draftChanged;
    void refresh();
    bool press(int page, QPointF point);
    void finish();
    void cancel();
    void reposition();
    bool focusNext(bool next);
    std::optional<Qt::CursorShape> cursorAt(int page, QPointF point) const;
    bool active() const
    {
        return !editor.isNull();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Document* document;
    Canvas* canvas;
    QVector<FormField> fields;
    quint64 revision = std::numeric_limits<quint64>::max(), editRevision = 0;
    FormField field;
    QPointer<QWidget> editor;
    bool preedit = false, finishing = false;
    QStringList values() const;
    void open(const FormField& selected);
    void updateDraft();
    void showError(const std::exception& error);
};
} // namespace tatsu
