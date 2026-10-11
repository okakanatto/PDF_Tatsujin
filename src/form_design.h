#pragma once
#include "form_fields.h"
#include <functional>

namespace tatsu
{
struct FormDesignWidget
{
    QString id;
    int page = 0;
    QRectF rectangle; // Visible, rotated page in physical points; top-left origin.
    bool operator==(const FormDesignWidget&) const = default;
};
struct FormDesignEntry
{
    QString id, name, caption;
    FormKind kind = FormKind::Text;
    QVector<FormDesignWidget> widgets;
    QStringList values, exports, labels;
    bool readOnly = false, required = false, editableChoice = false, multiple = false;
    int maxLength = 0;
    bool operator==(const FormDesignEntry&) const = default;
};
QVector<FormDesignEntry> formDesign(const PDFDocument& document,
                                    const std::function<bool()>& cancelled = {});
PDFDocument replaceFormDesign(const PDFDocument& document, const QVector<FormDesignEntry>& entries,
                              const std::function<bool()>& cancelled = {},
                              const std::function<void(int, int)>& progress = {});
} // namespace tatsu
