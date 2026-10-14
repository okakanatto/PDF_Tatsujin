#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
enum class DecorationKind
{
    HeaderFooter,
    Watermark
};
struct DecorationOptions
{
    DecorationKind kind = DecorationKind::HeaderFooter;
    QStringList header = {"", "", ""}, footer = {"", "", ""};
    QString watermark;
    QString fontFamily;
    double size = 12;
    QColor color = QColor("#444444");
    QMarginsF margins = {10, 10, 10, 10};
    int startNumber = 1;
    double angle = -30, opacity = .2;
};
struct DecorationGroup
{
    QString id;
    DecorationOptions options;
    QVector<int> pages;
};
QVector<DecorationGroup> decorationGroups(const PDFDocument& document);
PDFDocument putDecoration(const PDFDocument& document, const QVector<int>& pages,
                          const DecorationOptions& options, const QString& group = {},
                          const std::function<bool()>& cancelled = {},
                          const std::function<void(int, int)>& progress = {});
PDFDocument previewDecoration(const PDFDocument& document, const QVector<int>& pages,
                              int previewPage, const DecorationOptions& options,
                              const QString& group = {},
                              const std::function<bool()>& cancelled = {});
PDFDocument removeDecoration(const PDFDocument& document, const QString& group,
                             const std::function<bool()>& cancelled = {});
} // namespace tatsu
