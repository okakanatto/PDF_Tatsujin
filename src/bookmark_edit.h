#pragma once
#include "navigation.h"
#include <functional>

namespace tatsu
{
struct BookmarkEntry
{
    PDFObjectReference source;
    QString title;
    int parent = -1;
    int page = -1;
    bool changeDestination = false;
    bool expanded = true;
};
QVector<BookmarkEntry> editableBookmarks(const PDFDocument& document);
PDFDocument replaceBookmarks(const PDFDocument& document, const QVector<BookmarkEntry>& entries,
                             const std::function<bool()>& cancelled = {});
} // namespace tatsu
