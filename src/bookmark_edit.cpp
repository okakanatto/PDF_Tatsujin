#include "bookmark_edit.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include <set>

namespace tatsu
{
using namespace detail;
namespace
{
void valid(const PDFDocument& document)
{
    if (!document.getCatalog() || !document.getCatalog()->getPageCount() ||
        !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
}
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("しおりの変更を中止しました。文書は変更していません。");
}
} // namespace
QVector<BookmarkEntry> editableBookmarks(const PDFDocument& document)
{
    valid(document);
    const auto root = document.getObject(document.getTrailerDictionary()->get("Root"));
    const auto outline = root.getDictionary()->get("Outlines");
    if (outline.isNull())
        return {};
    if (!outline.isReference() || !document.getObject(outline).isDictionary())
        fail("しおりの構造に対応できません。部分編集は行いません。");
    QVector<BookmarkEntry> result;
    std::set<PDFObjectReference> visited;
    const PDFDocumentDataLoaderDecorator loader(&document);
    std::function<void(PDFObjectReference, int, int)> walk;
    walk = [&](PDFObjectReference reference, int parent, int depth)
    {
        if (depth > 32)
            fail("しおりの編集は32階層までです。残りを削除しません。");
        const auto object = document.getObjectByReference(reference);
        if (!object.isDictionary())
            fail("しおりの参照が不正です。文書は変更していません。");
        const auto dictionary = object.getDictionary();
        auto next = dictionary->get("First");
        PDFObject previous;
        while (!next.isNull())
        {
            if (!next.isReference() || visited.contains(next.getReference()) ||
                result.size() >= 1000)
                fail("しおりに循環・重複・上限超過があります。部分編集は行いません。");
            visited.insert(next.getReference());
            const auto value = document.getObject(next);
            if (!value.isDictionary())
                fail("しおりの項目を読み込めません。");
            const auto entry = value.getDictionary();
            if (entry->get("Parent") != PDFObject::createReference(reference) ||
                entry->get("Prev") != previous)
                fail("しおりの親子・兄弟関係が不正です。文書は変更していません。");
            const auto target = resolveActionObject(document, entry->get("A"), entry->get("Dest"));
            const auto count = document.getObject(entry->get("Count"));
            const bool expanded = count.isNull() || (count.isInt() && count.getInteger() >= 0);
            const int index = result.size();
            result << BookmarkEntry{next.getReference(),
                                    loader.readTextString(entry->get("Title"), {}),
                                    parent,
                                    target.valid() ? target.page : -1,
                                    false,
                                    expanded};
            if (!entry->get("First").isNull() || !entry->get("Last").isNull())
                walk(next.getReference(), index, depth + 1);
            previous = next;
            next = entry->get("Next");
        }
        if (dictionary->get("Last") != previous)
            fail("しおりの末尾参照が不正です。文書は変更していません。");
    };
    walk(outline.getReference(), -1, 1);
    return result;
}
PDFDocument replaceBookmarks(const PDFDocument& document, const QVector<BookmarkEntry>& entries,
                             const std::function<bool()>& cancelled)
{
    valid(document);
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    stop(cancelled);
    const auto existing = editableBookmarks(document);
    if (entries.size() > 1000)
        fail("しおりの編集は1000項目までです。");
    std::set<PDFObjectReference> allowed, seen;
    for (const auto& item : existing)
        allowed.insert(item.source);
    QVector<QVector<int>> children(entries.size() + 1);
    QVector<int> ancestors{-1};
    for (int i = 0; i < entries.size(); ++i)
    {
        stop(cancelled);
        const auto& item = entries[i];
        if (item.title.trimmed().isEmpty() || item.title.size() > 200)
            fail("しおりの名前は1〜200文字で指定してください。");
        for (auto character : item.title.toUcs4())
            if (character < 32 || character == 0x2028 || character == 0x2029)
                fail("しおりの名前に改行・制御文字は使えません。");
        if (item.parent < -1 || item.parent >= i || !ancestors.contains(item.parent))
            fail("しおりの階層と順序が不正です。");
        while (ancestors.back() != item.parent)
            ancestors.removeLast();
        ancestors << i;
        if (ancestors.size() > 33)
            fail("しおりの編集は32階層までです。");
        if (item.source.isValid())
        {
            if (!allowed.contains(item.source) || seen.contains(item.source))
                fail("変更するしおりが重複しているか見つかりません。");
            seen.insert(item.source);
        }
        else if (!item.changeDestination)
            fail("新しいしおりには移動先ページが必要です。");
        if (item.changeDestination &&
            (item.page < 0 || item.page >= int(document.getCatalog()->getPageCount())))
            fail("しおりの移動先ページが不正です。");
        children[item.parent + 1] << i;
    }
    PDFDocumentBuilder builder(&document);
    const auto catalogReference = builder.getCatalogReference();
    auto catalog = *builder.getObjectByReference(catalogReference).getDictionary();
    if (entries.isEmpty())
    {
        if (catalog.get("Outlines").isNull())
            return document;
        catalog.removeEntry("Outlines");
        builder.setObject(catalogReference, dictObject(catalog));
        stop(cancelled);
        return builder.build();
    }
    const auto oldOutline = catalog.get("Outlines");
    auto outline = oldOutline.isReference() ? oldOutline.getReference() : PDFObjectReference{};
    PDFDictionary root;
    if (outline.isValid())
        root = *builder.getObjectByReference(outline).getDictionary();
    else
    {
        set(root, "Type", PDFObject::createName("Outlines"));
        outline = builder.addObject(dictObject(root));
    }
    QVector<PDFObjectReference> references;
    for (const auto& item : entries)
        references << (item.source.isValid() ? item.source : builder.addObject(dictObject({})));
    QVector<int> visible(entries.size());
    for (int i = entries.size() - 1; i >= 0; --i)
        for (int child : children[i + 1])
            visible[i] += 1 + (entries[child].expanded ? visible[child] : 0);
    for (int i = 0; i < entries.size(); ++i)
    {
        stop(cancelled);
        const auto& item = entries[i];
        auto dictionary = item.source.isValid()
                              ? *builder.getObjectByReference(item.source).getDictionary()
                              : PDFDictionary{};
        for (const char* key : {"Parent", "First", "Last", "Prev", "Next", "Count"})
            dictionary.removeEntry(key);
        set(dictionary, "Title", PDFObjectFactory::createTextString(item.title));
        set(dictionary, "Parent",
            PDFObject::createReference(item.parent < 0 ? outline : references[item.parent]));
        const auto& siblings = children[item.parent + 1];
        const int position = siblings.indexOf(i);
        if (position > 0)
            set(dictionary, "Prev", PDFObject::createReference(references[siblings[position - 1]]));
        if (position + 1 < siblings.size())
            set(dictionary, "Next", PDFObject::createReference(references[siblings[position + 1]]));
        const auto& nested = children[i + 1];
        if (!nested.isEmpty())
        {
            set(dictionary, "First", PDFObject::createReference(references[nested.front()]));
            set(dictionary, "Last", PDFObject::createReference(references[nested.back()]));
            set(dictionary, "Count",
                PDFObject::createInteger(item.expanded ? visible[i] : -visible[i]));
        }
        if (item.changeDestination)
        {
            dictionary.removeEntry("A");
            set(dictionary, "Dest",
                arrObject({PDFObject::createReference(
                               document.getCatalog()->getPage(item.page)->getPageReference()),
                           PDFObject::createName("Fit")}));
        }
        builder.setObject(references[i], dictObject(dictionary));
    }
    const auto& top = children[0];
    set(root, "First", PDFObject::createReference(references[top.front()]));
    set(root, "Last", PDFObject::createReference(references[top.back()]));
    int count = 0;
    for (int item : top)
        count += 1 + (entries[item].expanded ? visible[item] : 0);
    set(root, "Count", PDFObject::createInteger(count));
    builder.setObject(outline, dictObject(root));
    set(catalog, "Outlines", PDFObject::createReference(outline));
    builder.setObject(catalogReference, dictObject(catalog));
    stop(cancelled);
    return builder.build();
}
} // namespace tatsu
