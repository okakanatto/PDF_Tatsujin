#pragma once
#include "navigation.h"
#include <functional>

namespace tatsu
{
enum class LinkTarget
{
    Preserve,
    Page,
    Web
};
struct LinkEntry
{
    int page = 0;
    int sourceIndex = -1;
    QRectF rectangle; // Rotated visible page, physical points, origin at top left.
    QString description;
    LinkTarget target = LinkTarget::Preserve;
    int destination = 0;
    QString url;
};
QVector<LinkEntry> editableLinks(const PDFDocument& document,
                                 const std::function<bool()>& cancelled = {});
PDFDocument replaceLinks(const PDFDocument& document, const QVector<LinkEntry>& entries,
                         const std::function<bool()>& cancelled = {});
QString normalizedLinkUrl(const QString& text);
} // namespace tatsu
