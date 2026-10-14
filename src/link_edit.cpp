#include "link_edit.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include <cmath>
#include <map>
#include <set>

namespace tatsu
{
using namespace detail;
namespace
{
using Key = std::pair<int, int>;
void valid(const PDFDocument& document)
{
    if (!document.getCatalog() || !document.getCatalog()->getPageCount() ||
        !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
}
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("リンクの変更を中止しました。文書は変更していません。");
}
std::vector<PDFObject> annotations(const PDFDocument& document, int page)
{
    const auto pageObject =
        document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
    const auto value = document.getObject(pageObject.getDictionary()->get("Annots"));
    if (value.isNull())
        return {};
    if (!value.isArray())
        fail("注釈の配列を読み込めません。部分編集は行いません。");
    std::vector<PDFObject> result;
    for (size_t i = 0; i < value.getArray()->getCount(); ++i)
    {
        const auto entry = value.getArray()->getItem(i);
        if (!document.getObject(entry).isDictionary())
            fail("注釈の参照が不正です。部分編集は行いません。");
        result.push_back(entry);
    }
    return result;
}
bool link(const PDFDocument& document, const PDFObject& object)
{
    const auto subtype =
        document.getObject(document.getObject(object).getDictionary()->get("Subtype"));
    return subtype.isName() && subtype.getString() == "Link";
}
QRectF rectangle(const PDFDocument& document, const PDFDictionary* dictionary)
{
    const auto value = document.getObject(dictionary->get("Rect"));
    if (!value.isArray() || value.getArray()->getCount() != 4)
        fail("リンクの矩形が不正です。");
    double values[4];
    for (int i = 0; i < 4; ++i)
    {
        const auto v = document.getObject(value.getArray()->getItem(i));
        if ((!v.isInt() && !v.isReal()) ||
            !std::isfinite(v.isInt() ? double(v.getInteger()) : v.getReal()))
            fail("リンクの座標が不正です。");
        values[i] = v.isInt() ? double(v.getInteger()) : v.getReal();
    }
    const QRectF result(QPointF(values[0], values[1]), QPointF(values[2], values[3]));
    if (result.width() <= 0 || result.height() <= 0)
        fail("リンクの幅・高さが不正です。");
    return result;
}
bool sameRectangle(QRectF a, QRectF b)
{
    return std::abs(a.x() - b.x()) < 1e-10 && std::abs(a.y() - b.y()) < 1e-10 &&
           std::abs(a.width() - b.width()) < 1e-10 && std::abs(a.height() - b.height()) < 1e-10;
}
void moveQuads(const PDFDocument& document, PDFDictionary& dictionary, QRectF oldBox, QRectF box)
{
    const auto value = document.getObject(dictionary.get("QuadPoints"));
    if (value.isNull())
        return;
    if (!value.isArray() || !value.getArray()->getCount() || value.getArray()->getCount() % 8)
        fail("リンクのQuadPointsが不正なため範囲を変更できません。");
    std::vector<PDFObject> moved;
    for (size_t i = 0; i < value.getArray()->getCount(); ++i)
    {
        const auto v = document.getObject(value.getArray()->getItem(i));
        if ((!v.isInt() && !v.isReal()) ||
            !std::isfinite(v.isInt() ? double(v.getInteger()) : v.getReal()))
            fail("リンクのQuadPoints座標が不正です。");
        const bool x = i % 2 == 0;
        const double origin = x ? oldBox.x() : oldBox.y();
        const double span = x ? oldBox.width() : oldBox.height();
        const double normalized =
            ((v.isInt() ? double(v.getInteger()) : v.getReal()) - origin) / span;
        if (normalized < -1e-8 || normalized > 1 + 1e-8)
            fail("リンクのQuadPointsが矩形の外にあります。範囲は変更しません。");
        moved.push_back(
            number((x ? box.x() : box.y()) + normalized * (x ? box.width() : box.height())));
    }
    set(dictionary, "QuadPoints", arrObject(std::move(moved)));
}
} // namespace
QString normalizedLinkUrl(const QString& text)
{
    if (text != text.trimmed() || text.size() > 4096)
        fail("URLは前後の空白を除き4096文字以内で指定してください。");
    for (auto c : text.toUcs4())
        if (c <= 32 || c == 127 || c == 0x2028 || c == 0x2029)
            fail("URLに空白・改行・制御文字は使えません。");
    const QUrl value(text, QUrl::StrictMode);
    if (!value.isValid() ||
        (value.scheme().toLower() != "http" && value.scheme().toLower() != "https") ||
        value.host().isEmpty() || !value.userInfo().isEmpty())
        fail("資格情報を含まないhttpまたはhttpsのURLを指定してください。");
    return QString::fromLatin1(value.toEncoded(QUrl::FullyEncoded));
}
QVector<LinkEntry> editableLinks(const PDFDocument& document,
                                 const std::function<bool()>& cancelled)
{
    valid(document);
    QVector<LinkEntry> result;
    std::set<PDFObjectReference> references;
    const PDFDocumentDataLoaderDecorator loader(&document);
    for (int page = 0; page < int(document.getCatalog()->getPageCount()); ++page)
    {
        stop(cancelled);
        const auto items = annotations(document, page);
        for (int index = 0; index < int(items.size()); ++index)
        {
            const auto& item = items[index];
            if (!link(document, item))
                continue;
            if (result.size() >= 1000 ||
                (item.isReference() && !references.insert(item.getReference()).second))
                fail("リンクに重複・上限超過があります。部分編集は行いません。");
            const auto dictionary = document.getObject(item).getDictionary();
            const auto target =
                resolveActionObject(document, dictionary->get("A"), dictionary->get("Dest"));
            LinkEntry entry;
            entry.page = page;
            entry.sourceIndex = index;
            entry.rectangle = pageMatrix(document.getCatalog()->getPage(page))
                                  .mapRect(rectangle(document, dictionary));
            for (double value : {entry.rectangle.x(), entry.rectangle.y(), entry.rectangle.width(),
                                 entry.rectangle.height()})
                if (!std::isfinite(value))
                    fail("リンクのページ座標が不正です。");
            if (entry.rectangle.width() <= 0 || entry.rectangle.height() <= 0)
                fail("リンクのページ倍率が不正です。");
            entry.description = loader.readTextString(dictionary->get("Contents"), {});
            entry.destination = target.valid() ? target.page : 0;
            const auto action = document.getObject(dictionary->get("A"));
            if (action.isDictionary())
                entry.url =
                    QString::fromUtf8(loader.readString(action.getDictionary()->get("URI")));
            result << entry;
        }
    }
    return result;
}
PDFDocument replaceLinks(const PDFDocument& document, const QVector<LinkEntry>& entries,
                         const std::function<bool()>& cancelled)
{
    valid(document);
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    stop(cancelled);
    const auto existing = editableLinks(document, cancelled);
    if (entries.size() > 1000)
        fail("リンクの編集は1000項目までです。");
    std::map<Key, LinkEntry> originals;
    for (const auto& item : existing)
        originals.emplace(Key{item.page, item.sourceIndex}, item);
    std::map<Key, int> positions;
    QVector<QString> urls(entries.size());
    for (int i = 0; i < entries.size(); ++i)
    {
        stop(cancelled);
        const auto& item = entries[i];
        if (item.page < 0 || item.page >= int(document.getCatalog()->getPageCount()) ||
            item.sourceIndex < -1)
            fail("リンクの配置ページが不正です。");
        const Key key{item.page, item.sourceIndex};
        if (item.sourceIndex >= 0 &&
            (!originals.contains(key) || !positions.emplace(key, i).second))
            fail("変更するリンクが重複しているか見つかりません。");
        const auto old = originals.find(key);
        const bool newItem = item.sourceIndex < 0;
        const bool moved = newItem || !sameRectangle(item.rectangle, old->second.rectangle);
        if (moved)
        {
            const auto& box = item.rectangle;
            const auto size = pageSize(document.getCatalog()->getPage(item.page));
            for (double number : {box.x(), box.y(), box.width(), box.height()})
                if (!std::isfinite(number))
                    fail("リンクの範囲が不正です。");
            if (box.width() < 1 || box.height() < 1 || box.x() < -1e-8 || box.y() < -1e-8 ||
                box.right() > size.width() + 1e-8 || box.bottom() > size.height() + 1e-8)
                fail("リンクは用紙の表示範囲内で幅・高さ1pt以上にしてください。");
        }
        if (newItem || item.description != old->second.description)
        {
            if (item.description.size() > 500)
                fail("リンクの説明は500文字以内で指定してください。");
            for (auto c : item.description.toUcs4())
                if (c < 32 || c == 0x2028 || c == 0x2029)
                    fail("リンクの説明に改行・制御文字は使えません。");
        }
        switch (item.target)
        {
        case LinkTarget::Preserve:
            if (newItem)
                fail("新しいリンクには移動先が必要です。");
            break;
        case LinkTarget::Page:
            if (item.destination < 0 ||
                item.destination >= int(document.getCatalog()->getPageCount()))
                fail("リンクの移動先ページが不正です。");
            break;
        case LinkTarget::Web:
            urls[i] = normalizedLinkUrl(item.url);
            break;
        default:
            fail("リンクの移動先形式が不正です。");
        }
    }
    PDFDocumentBuilder builder(&document);
    bool changed = false;
    auto update = [&](const PDFObject& source, const LinkEntry& item, int index)
    {
        stop(cancelled);
        const auto oldObject = document.getObject(source);
        auto dictionary = source.isNull() ? PDFDictionary{} : *oldObject.getDictionary();
        const auto page = document.getCatalog()->getPage(item.page);
        const auto box = pageMatrix(page).inverted().mapRect(item.rectangle);
        const auto found = originals.find({item.page, item.sourceIndex});
        const bool moved =
            source.isNull() || !sameRectangle(item.rectangle, found->second.rectangle);
        if (source.isNull())
        {
            set(dictionary, "Type", PDFObject::createName("Annot"));
            set(dictionary, "Subtype", PDFObject::createName("Link"));
            set(dictionary, "P", PDFObject::createReference(page->getPageReference()));
            set(dictionary, "Border", arrObject({number(0), number(0), number(0)}));
            set(dictionary, "H", PDFObject::createName("I"));
        }
        else if (moved)
            moveQuads(document, dictionary, rectangle(document, oldObject.getDictionary()), box);
        if (moved)
            set(dictionary, "Rect", rectObject(box));
        if (source.isNull() || item.description != found->second.description)
            set(dictionary, "Contents", PDFObjectFactory::createTextString(item.description));
        if (item.target != LinkTarget::Preserve)
        {
            dictionary.removeEntry("A");
            dictionary.removeEntry("Dest");
            if (item.target == LinkTarget::Page)
                set(dictionary, "Dest",
                    arrObject(
                        {PDFObject::createReference(
                             document.getCatalog()->getPage(item.destination)->getPageReference()),
                         PDFObject::createName("Fit")}));
            else
            {
                PDFDictionary action;
                set(action, "S", PDFObject::createName("URI"));
                set(action, "URI", PDFObject::createString(urls[index].toUtf8()));
                set(dictionary, "A", dictObject(action));
            }
        }
        const auto result = dictObject(dictionary);
        if (result == oldObject)
            return source;
        changed = true;
        if (source.isReference())
        {
            builder.setObject(source.getReference(), result);
            return source;
        }
        return PDFObject::createReference(builder.addObject(result));
    };
    for (int page = 0; page < int(document.getCatalog()->getPageCount()); ++page)
    {
        stop(cancelled);
        const auto old = annotations(document, page);
        std::vector<PDFObject> kept;
        for (int index = 0; index < int(old.size()); ++index)
        {
            if (!link(document, old[index]))
                kept.push_back(old[index]);
            else if (const auto found = positions.find({page, index}); found != positions.end())
                kept.push_back(update(old[index], entries[found->second], found->second));
            else
                changed = true;
        }
        for (int i = 0; i < entries.size(); ++i)
            if (entries[i].page == page && entries[i].sourceIndex < 0)
                kept.push_back(update({}, entries[i], i));
        if (old != kept)
        {
            const auto reference = document.getCatalog()->getPage(page)->getPageReference();
            auto dictionary = *builder.getObjectByReference(reference).getDictionary();
            set(dictionary, "Annots", arrObject(std::move(kept)));
            builder.setObject(reference, dictObject(dictionary));
        }
    }
    stop(cancelled);
    return changed ? builder.build() : document;
}
} // namespace tatsu
