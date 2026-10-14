#pragma once
#include "document.h"

QJsonObject checkRedactionCandidate(pdf::PDFDocument document,
                                    const QMap<int, QVector<QRectF>>& regions,
                                    const QString& output);
