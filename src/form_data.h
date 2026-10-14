#pragma once
#include "form_fields.h"
#include <functional>

namespace tatsu
{
struct FormDataValue
{
    QString name;
    QStringList values;
    bool operator==(const FormDataValue&) const = default;
};
struct FormDataValues
{
    QVector<FormDataValue> fields;
    QStringList unsupported;
};
struct FormDataChange
{
    QString name, label;
    QStringList before, after;
};
struct FormDataImport
{
    PDFDocument document{};
    QVector<FormDataChange> changes;
};
FormDataValues formDataValues(const PDFDocument& document);
QByteArray encodeXfdf(const QVector<FormDataValue>& values);
QVector<FormDataValue> decodeXfdf(const QByteArray& data);
FormDataImport importFormData(const PDFDocument& document, const QVector<FormDataValue>& values,
                              const std::function<bool()>& cancelled = {},
                              const std::function<void(int, int)>& progress = {});
void exportFormData(const PDFDocument& document, const QString& destination,
                    const std::function<bool()>& cancelled = {});
} // namespace tatsu
