#pragma once
#include "document.h"
namespace tatsu
{
enum class FormKind
{
    Text,
    Multiline,
    Checkbox,
    Radio,
    Combo,
    List,
    Unsupported
};
struct FormField
{
    PDFObjectReference field, widget;
    int page = -1;
    QRectF rectangle;
    QString name, qualifiedName, notice;
    FormKind kind = FormKind::Unsupported;
    QStringList values, exports, labels;
    bool readOnly = false, editableChoice = false, multiple = false, noToggleOff = false;
    int maxLength = 0;
    QString onState;
    bool utf8ButtonNames = false; // Our owned button states; preserve foreign name bytes.
};
QVector<FormField> formFields(const PDFDocument& document);
void putFormValue(Document& document, PDFObjectReference widget, const QStringList& values);
} // namespace tatsu
