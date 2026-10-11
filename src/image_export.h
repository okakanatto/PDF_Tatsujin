#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct ImageExportOptions
{
    QString directory;
    QString prefix;
    QByteArray format = "png";
    int dpi = 150;
};
struct ImageExportResult
{
    int page = -1;
    QString path, error;
    bool success = false;
};
struct ImageExportOutcome
{
    QVector<ImageExportResult> files;
    bool cancelled = false;
};
QSize imageExportSize(const PDFDocument& document, int page, int dpi);
ImageExportOutcome exportImages(PDFDocument document, const QVector<int>& pages,
                                const ImageExportOptions& options,
                                const std::function<bool()>& cancelled = {},
                                const std::function<void(const QString&, int, int)>& progress = {});
} // namespace tatsu
