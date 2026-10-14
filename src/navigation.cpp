#include "navigation.h"
#include "pdfnumbertreeloader.h"
#include <cmath>
#include <set>

namespace tatsu
{
namespace
{
// The pinned Windows DLL does not export PDFPageLabel::parse. Keep the
// number-tree reader and decode these three fields in the application adapter.
struct LabelRange : PDFPageLabel
{
    using PDFPageLabel::PDFPageLabel;
    static LabelRange parse(PDFInteger page, const PDFObjectStorage* storage,
                            const PDFObject& object)
    {
        const auto dictionary = storage->getDictionaryFromObject(object);
        if (!dictionary)
            return LabelRange();
        const PDFDocumentDataLoaderDecorator loader(storage);
        const QMap<QByteArray, NumberingStyle> styles{{"D", NumberingStyle::DecimalArabic},
                                                      {"R", NumberingStyle::UppercaseRoman},
                                                      {"r", NumberingStyle::LowercaseRoman},
                                                      {"A", NumberingStyle::UppercaseLetters},
                                                      {"a", NumberingStyle::LowercaseLetters}};
        return LabelRange(styles.value(loader.readName(dictionary->get("S")), NumberingStyle::None),
                          loader.readTextString(dictionary->get("P"), {}), page,
                          loader.readInteger(dictionary->get("St"), 1));
    }
};
} // namespace
NavigationTarget resolveDestination(const PDFDocument& document, PDFDestination destination)
{
    NavigationTarget target;
    const auto catalog = document.getCatalog();
    std::set<QByteArray> names;
    while (destination.isNamedDestination())
    {
        const auto name = destination.getName();
        if (!names.insert(name).second || names.size() > 64)
        {
            target.notice = "名前付き宛先が循環しています。";
            return target;
        }
        const auto named = catalog->getNamedDestination(name);
        if (!named)
        {
            target.notice = "名前付き宛先を見つけられません。";
            return target;
        }
        destination = *named;
    }
    switch (destination.getDestinationType())
    {
    case DestinationType::XYZ:
    case DestinationType::Fit:
    case DestinationType::FitH:
    case DestinationType::FitV:
    case DestinationType::FitR:
        break;
    default:
        target.notice = "この宛先形式は未対応です。";
        return target;
    }
    const auto reference = destination.getPageReference();
    const auto number = reference.isValid() ? catalog->getPageIndexFromPageReference(reference)
                                            : size_t(destination.getPageIndex());
    if (number >= catalog->getPageCount())
    {
        target.notice = "宛先のページが文書内にありません。";
        return target;
    }
    for (const auto value : {destination.getLeft(), destination.getTop(), destination.getRight(),
                             destination.getBottom(), destination.getZoom()})
        if (std::isinf(value))
        {
            target.notice = "宛先の座標または倍率が無効です。";
            return target;
        }
    if (destination.getDestinationType() == DestinationType::FitR &&
        (!destination.hasLeft() || !destination.hasTop() || !destination.hasRight() ||
         !destination.hasBottom() || destination.getRight() <= destination.getLeft() ||
         destination.getTop() <= destination.getBottom()))
    {
        target.notice = "宛先の矩形が無効です。";
        return target;
    }
    target.page = int(number);
    target.destination = destination;
    return target;
}
NavigationTarget resolveAction(const PDFDocument& document, const PDFAction* action)
{
    if (!action)
        return {PDFDestination(), -1, "この項目には移動先がありません。"};
    if (!action->getNextActions().empty())
        return {PDFDestination(), -1, "複合アクションは未対応です。"};
    if (action->getType() == ActionType::GoTo)
        return resolveDestination(document,
                                  static_cast<const PDFActionGoTo*>(action)->getDestination());
    if (action->getType() == ActionType::URI)
        return {PDFDestination(), -1,
                "外部リンク：" +
                    QString::fromUtf8(static_cast<const PDFActionURI*>(action)->getURI()) +
                    "\nこのM1試作では外部リンクを開きません。"};
    return {PDFDestination(), -1,
            "このアクションは未対応です。外部ファイルやスクリプトは実行しません。"};
}
NavigationTarget resolveActionObject(const PDFDocument& document, const PDFObject& action,
                                     const PDFObject& destination)
{
    const auto storage = &document.getStorage();
    // PDF4QT's pinned action parser discards /Next. Inspect the original
    // dictionary so a compound action never silently runs just its first step.
    if (const auto dictionary = storage->getDictionaryFromObject(action))
        if (!dictionary->get("Next").isNull())
            return {PDFDestination(), -1, "複合アクションは未対応です。"};
    if (!action.isNull())
    {
        try
        {
            return resolveAction(document, PDFAction::parse(storage, action).get());
        }
        catch (const std::exception&)
        {
            return {PDFDestination(), -1, "アクションを読み込めません。"};
        }
    }
    if (!destination.isNull())
        return resolveDestination(document, PDFDestination::parse(storage, destination));
    return {PDFDestination(), -1, "この項目には移動先がありません。"};
}
NavigationTarget resolveLink(const PDFDocument& document, const PDFLinkAnnotation& link)
{
    const auto dictionary = document.getStorage().getDictionaryFromObject(
        PDFObject::createReference(link.getSelfReference()));
    return dictionary ? resolveActionObject(document, dictionary->get("A"), dictionary->get("Dest"))
                      : resolveAction(document, link.getAction());
}
BookmarkList readBookmarks(const PDFDocument& document)
{
    BookmarkList result;
    const auto storage = &document.getStorage();
    const PDFDocumentDataLoaderDecorator loader(storage);
    std::set<PDFObjectReference> visited;
    std::function<void(PDFObject, int, int)> walk;
    walk = [&](PDFObject object, int parent, int depth)
    {
        while (!object.isNull())
        {
            if (depth > 64 || result.items.size() >= 10000)
            {
                result.limited = true;
                return;
            }
            if (object.isReference() && !visited.insert(object.getReference()).second)
                return;
            const auto dictionary = storage->getDictionaryFromObject(object);
            if (!dictionary)
                return;
            const int index = result.items.size();
            result.items.append(
                {loader.readTextString(dictionary->get("Title"), {}),
                 resolveActionObject(document, dictionary->get("A"), dictionary->get("Dest")),
                 parent,
                 dictionary->get("First").isNull() ||
                     loader.readIntegerFromDictionary(dictionary, "Count", 0) > 0});
            walk(dictionary->get("First"), index, depth + 1);
            object = dictionary->get("Next");
        }
    };
    const auto catalog =
        storage->getDictionaryFromObject(document.getTrailerDictionary()->get("Root"));
    if (catalog)
        if (const auto outlines = storage->getDictionaryFromObject(catalog->get("Outlines")))
            walk(outlines->get("First"), -1, 0);
    return result;
}
QStringList readPageLabels(const PDFDocument& document)
{
    QStringList result(int(document.getCatalog()->getPageCount()), QString());
    auto storage = &document.getStorage();
    const auto catalog =
        storage->getDictionaryFromObject(document.getTrailerDictionary()->get("Root"));
    if (!catalog || catalog->get("PageLabels").isNull())
        return result;
    const auto ranges = PDFNumberTreeLoader<LabelRange>::parse(storage, catalog->get("PageLabels"));
    for (size_t index = 0; index < ranges.size(); ++index)
    {
        const auto& range = ranges[index];
        const auto start = range.getPageIndex();
        const auto end =
            index + 1 < ranges.size() ? ranges[index + 1].getPageIndex() : result.size();
        if (start < 0 || start >= result.size() || end <= start || range.getPageStartNumber() < 1)
            continue;
        for (int page = int(start); page < qMin<PDFInteger>(end, result.size()); ++page)
        {
            const auto offset = page - start;
            if (range.getPageStartNumber() > std::numeric_limits<PDFInteger>::max() - offset)
                continue;
            auto number = range.getPageStartNumber() + offset;
            QString text;
            switch (range.getNumberingStyle())
            {
            case PDFPageLabel::NumberingStyle::None:
                break;
            case PDFPageLabel::NumberingStyle::DecimalArabic:
                text = QString::number(number);
                break;
            case PDFPageLabel::NumberingStyle::UppercaseRoman:
            case PDFPageLabel::NumberingStyle::LowercaseRoman:
                if (number < 1 || number > 100000)
                    continue;
                for (const auto& pair : {std::pair{1000, "M"},
                                         {900, "CM"},
                                         {500, "D"},
                                         {400, "CD"},
                                         {100, "C"},
                                         {90, "XC"},
                                         {50, "L"},
                                         {40, "XL"},
                                         {10, "X"},
                                         {9, "IX"},
                                         {5, "V"},
                                         {4, "IV"},
                                         {1, "I"}})
                    while (number >= pair.first)
                    {
                        text += QLatin1String(pair.second);
                        number -= pair.first;
                    }
                if (range.getNumberingStyle() == PDFPageLabel::NumberingStyle::LowercaseRoman)
                    text = text.toLower();
                break;
            case PDFPageLabel::NumberingStyle::UppercaseLetters:
            case PDFPageLabel::NumberingStyle::LowercaseLetters:
                if (number < 1 || number > 2600)
                    continue;
                text = QString(int((number - 1) / 26 + 1), QChar('A' + int((number - 1) % 26)));
                if (range.getNumberingStyle() == PDFPageLabel::NumberingStyle::LowercaseLetters)
                    text = text.toLower();
                break;
            }
            result[page] = range.getPrefix() + text;
        }
    }
    return result;
}
QString pageDescription(int page, const QStringList& labels)
{
    const auto physical = QString("%1 ページ").arg(page + 1);
    return page >= 0 && page < labels.size() && !labels[page].isEmpty()
               ? labels[page] + "（" + physical + "）"
               : physical;
}
} // namespace tatsu
