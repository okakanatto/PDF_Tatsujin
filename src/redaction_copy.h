#pragma once
#include "redaction_document.h"

namespace tatsu
{
struct RedactedCopy
{
    QByteArray hash;
    int textSegments = 0, images = 0, fieldGroups = 0, annotations = 0;
    int contentGroups = 0;
};
// Publish a fresh copy without committing any change to the caller's document.
RedactedCopy exportRedactedPdf(const PDFDocument& document,
                               const QMap<int, QVector<QRectF>>& regions,
                               const QString& destination,
                               const std::function<bool()>& cancelled = {},
                               const std::function<void(QString)>& progress = {},
                               const std::function<void()>& validateInputs = {});
} // namespace tatsu
