#pragma once
#include "document.h"
namespace tatsu
{
PDFDocument selectPages(const PDFDocument& document, const QVector<int>& order);
PDFDocument insertPages(const PDFDocument& document, const PDFDocument& incoming,
                        const QVector<int>& pages, int before);
PDFDocument mergeDocuments(const QVector<PDFDocument>& documents);
QVector<int> movePageOrder(int count, QVector<int> selected, int before);
struct ExportResult
{
    QString path, error;
    bool success = false;
};
QVector<ExportResult> exportPageGroups(const PDFDocument& document,
                                       const QVector<QVector<int>>& groups,
                                       const QStringList& destinations);
} // namespace tatsu
