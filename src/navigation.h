#pragma once
#include "document.h"
#include "pdfaction.h"
#include "pdfannotation.h"

namespace tatsu
{
struct NavigationTarget
{
    PDFDestination destination;
    int page = -1;
    QString notice;
    bool valid() const
    {
        return page >= 0 && notice.isEmpty();
    }
};
NavigationTarget resolveDestination(const PDFDocument& document, PDFDestination destination);
NavigationTarget resolveAction(const PDFDocument& document, const PDFAction* action);
NavigationTarget resolveActionObject(const PDFDocument& document, const PDFObject& action,
                                     const PDFObject& destination);
NavigationTarget resolveLink(const PDFDocument& document, const PDFLinkAnnotation& link);
struct NavigationBookmark
{
    QString title;
    NavigationTarget target;
    int parent = -1;
    bool expanded = true;
};
struct BookmarkList
{
    QVector<NavigationBookmark> items;
    bool limited = false;
};
BookmarkList readBookmarks(const PDFDocument& document);
QStringList readPageLabels(const PDFDocument& document);
QString pageDescription(int page, const QStringList& labels);
} // namespace tatsu
