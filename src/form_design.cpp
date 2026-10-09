#include "form_design.h"
#include "form_font.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfform.h"
#include <cmath>
#include <map>
#include <set>

namespace tatsu
{
using namespace detail;
namespace
{
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("フォームの設計を中止しました。元の文書は変更していません。");
}
void valid(const PDFDocument& document)
{
    if (!document.getCatalog() || !document.getCatalog()->getPageCount())
        fail("PDFを開いてください。");
}
std::vector<PDFObject> array(const PDFDocument& document, const PDFObject& object)
{
    const auto value = document.getObject(object);
    if (value.isNull())
        return {};
    if (!value.isArray())
        fail("フォームまたは注釈の配列が不正です。部分編集は行いません。");
    if (value.getArray()->getCount() > 100000)
        fail("フォームまたは注釈の構造が大きすぎます。");
    std::vector<PDFObject> result;
    for (const auto& entry : *value.getArray())
        result.push_back(entry);
    return result;
}
PDFDictionary formDictionary(const PDFDocument& document)
{
    const auto object = document.getObject(document.getCatalog()->getFormObject());
    if (object.isNull())
        return {};
    if (!object.isDictionary())
        fail("AcroFormの構造が不正です。");
    return *object.getDictionary();
}
QJsonObject metadata(const PDFObject& object, const char* key)
{
    if (!object.isDictionary())
        fail("フォームの参照が不正です。");
    const auto value = object.getDictionary()->get(key);
    if (value.isNull())
        return {};
    if (!value.isString() || value.getString().size() > 16 * 1024 * 1024)
        fail("フォームの再設計情報が不正です。");
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(value.getString(), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject() ||
        json.object()["version"].toInt() != 1)
        fail("このフォームの再設計情報には対応していません。");
    return json.object();
}
void validateFieldTree(const PDFDocument& document, const std::function<bool()>& cancelled)
{
    // Validate the raw graph before entering the SDK's recursive form parser.
    // This also covers foreign hierarchy and hidden fields that have no widget.
    std::set<PDFObjectReference> seen;
    int count = 0;
    std::function<void(const PDFObject&, PDFObjectReference, int)> walk =
        [&](const PDFObject& entry, PDFObjectReference parent, int depth)
    {
        stop(cancelled);
        if (!entry.isReference() || depth > 32 || ++count > 100000 ||
            seen.contains(entry.getReference()))
            fail("既存フォームに循環・重複参照・上限超過があります。");
        seen.insert(entry.getReference());
        const auto object = document.getObject(entry);
        if (!object.isDictionary())
            fail("既存フォームの項目参照が不正です。");
        const auto dictionary = object.getDictionary();
        const auto actualParent = dictionary->get("Parent");
        if ((parent.isValid() && actualParent != PDFObject::createReference(parent)) ||
            (!parent.isValid() && !actualParent.isNull()))
            fail("既存フォームの親子参照が不整合です。");
        const auto subtype = document.getObject(dictionary->get("Subtype"));
        const auto kids = array(document, dictionary->get("Kids"));
        if (subtype.isName() && subtype.getString() == "Widget" && !kids.empty())
            fail("widgetに子項目があるフォームの再設計には対応していません。");
        for (const auto& child : kids)
            walk(child, entry.getReference(), depth + 1);
    };
    for (const auto& entry : array(document, formDictionary(document).get("Fields")))
        walk(entry, {}, 0);
}
bool owned(const PDFDocument& document, const PDFObject& field)
{
    return !metadata(document.getObject(field), "TatsujinForm").isEmpty();
}
bool idValid(const QString& id)
{
    return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
void textValid(const QString& text, int maximum, bool multiline)
{
    if (text.size() > maximum)
        fail("フォームの文字数が上限を超えています。");
    for (int i = 0; i < text.size(); ++i)
    {
        const auto c = text[i];
        if (c.isHighSurrogate())
        {
            if (++i >= text.size() || !text[i].isLowSurrogate())
                fail("フォームに不正なUnicode文字があります。");
        }
        else if (c.isLowSurrogate() || c.unicode() == 0 ||
                 (c.category() == QChar::Other_Control &&
                  !(multiline && (c == '\r' || c == '\n'))) ||
                 (!multiline && (c == QChar(0x2028) || c == QChar(0x2029))))
            fail("フォームに使用できない制御文字があります。");
    }
}
PDFObject valueObject(const FormDesignEntry& entry)
{
    if (entry.kind == FormKind::List && entry.values.isEmpty())
        return arrObject({});
    if (entry.kind == FormKind::Checkbox || entry.kind == FormKind::Radio)
        return PDFObject::createName(entry.values.value(0, "Off").toUtf8());
    if (entry.values.size() <= 1)
        return PDFObjectFactory::createTextString(entry.values.value(0));
    std::vector<PDFObject> values;
    for (const auto& text : entry.values)
        values.push_back(PDFObjectFactory::createTextString(text));
    return arrObject(std::move(values));
}
PDFObjectReference buttonAppearance(PDFDocumentBuilder& builder, QSizeF size, bool on, bool radio,
                                    int rotation)
{
    const double w = size.width(), h = size.height();
    auto n = [](double v) { return QByteArray::number(v, 'f', 8); };
    QByteArray commands = "q 1 g 0 0 " + n(w) + ' ' + n(h) + " re f 0.3 G 0.8 w\n";
    const double left = .8, bottom = .8, right = w - .8, top = h - .8;
    if (radio)
    {
        const double cx = w / 2, cy = h / 2, radius = qMin(w, h) / 2 - 1;
        auto circle = [&](double r)
        {
            const double k = r * .5522847498307936;
            commands += n(cx + r) + ' ' + n(cy) + " m " + n(cx + r) + ' ' + n(cy + k) + ' ' +
                        n(cx + k) + ' ' + n(cy + r) + ' ' + n(cx) + ' ' + n(cy + r) + " c " +
                        n(cx - k) + ' ' + n(cy + r) + ' ' + n(cx - r) + ' ' + n(cy + k) + ' ' +
                        n(cx - r) + ' ' + n(cy) + " c " + n(cx - r) + ' ' + n(cy - k) + ' ' +
                        n(cx - k) + ' ' + n(cy - r) + ' ' + n(cx) + ' ' + n(cy - r) + " c " +
                        n(cx + k) + ' ' + n(cy - r) + ' ' + n(cx + r) + ' ' + n(cy - k) + ' ' +
                        n(cx + r) + ' ' + n(cy) + " c ";
        };
        circle(radius);
        commands += "S\n";
        if (on)
        {
            commands += "0 g ";
            circle(radius * .5);
            commands += "f\n";
        }
    }
    else
    {
        commands +=
            n(left) + ' ' + n(bottom) + ' ' + n(right - left) + ' ' + n(top - bottom) + " re S\n";
        if (on)
            commands += "0 G 1.4 w " + n(w * .2) + ' ' + n(h * .5) + " m " + n(w * .42) + ' ' +
                        n(h * .25) + " l " + n(w * .82) + ' ' + n(h * .8) + " l S\n";
    }
    commands += "Q\n";
    PDFDictionary appearance;
    set(appearance, "Type", PDFObject::createName("XObject"));
    set(appearance, "Subtype", PDFObject::createName("Form"));
    set(appearance, "BBox", rectObject(QRectF(QPointF(), size)));
    set(appearance, "Resources", dictObject({}));
    if (rotation)
    {
        QTransform matrix;
        matrix.rotate(rotation);
        const auto box = matrix.mapRect(QRectF(QPointF(), size));
        set(appearance, "Matrix",
            arrObject({number(matrix.m11()), number(matrix.m12()), number(matrix.m21()),
                       number(matrix.m22()), number(-box.x()), number(-box.y())}));
    }
    return builder.addObject(streamObject(appearance, commands));
}
struct Existing
{
    PDFObjectReference field;
    std::map<QString, PDFObjectReference> widgets;
};
std::map<QString, Existing> existing(const PDFDocument& document,
                                     const std::function<bool()>& cancelled)
{
    std::map<QString, Existing> result;
    QSet<QString> ids;
    const auto form = formDictionary(document);
    for (const auto& entry : array(document, form.get("Fields")))
    {
        stop(cancelled);
        const auto object = document.getObject(entry);
        const auto meta = metadata(object, "TatsujinForm");
        if (meta.isEmpty())
            continue;
        const auto id = meta["id"].toString();
        if (!entry.isReference() || !idValid(id) || ids.contains(id))
            fail("再設計するフォームに重複または不正なIDがあります。");
        ids.insert(id);
        Existing value;
        value.field = entry.getReference();
        for (const auto& child : array(document, object.getDictionary()->get("Kids")))
        {
            const auto widget = document.getObject(child);
            const auto marker = metadata(widget, "TatsujinWidget");
            const auto widgetId = marker["id"].toString();
            if (!child.isReference() || !idValid(widgetId) || marker["field"].toString() != id ||
                ids.contains(widgetId) || widget.getDictionary()->get("Parent") != entry)
                fail("フォームの親子関係が不正です。部分編集は行いません。");
            ids.insert(widgetId);
            value.widgets.emplace(widgetId, child.getReference());
        }
        result.emplace(id, std::move(value));
    }
    return result;
}
} // namespace
QVector<FormDesignEntry> formDesign(const PDFDocument& document,
                                    const std::function<bool()>& cancelled)
{
    valid(document);
    validateFieldTree(document, cancelled);
    const auto originals = existing(document, cancelled);
    const auto fields = formFields(document);
    const PDFDocumentDataLoaderDecorator loader(&document);
    QVector<FormDesignEntry> result;
    int widgetCount = 0;
    std::set<PDFObjectReference> encountered;
    for (const auto& [id, source] : originals)
    {
        stop(cancelled);
        FormDesignEntry entry;
        entry.id = id;
        const auto object = document.getObjectByReference(source.field);
        const auto dictionary = object.getDictionary();
        entry.name = loader.readTextString(dictionary->get("T"), {});
        entry.caption = loader.readTextString(dictionary->get("TU"), {});
        const auto flags = loader.readIntegerFromDictionary(dictionary, "Ff", 0);
        entry.readOnly = flags & 1;
        entry.required = flags & 2;
        entry.editableChoice = flags & (1 << 18);
        entry.multiple = flags & (1 << 21);
        entry.maxLength = int(loader.readIntegerFromDictionary(dictionary, "MaxLen", 0));
        for (const auto& child : array(document, dictionary->get("Kids")))
        {
            stop(cancelled);
            const auto found = std::find_if(
                fields.begin(), fields.end(), [&](const auto& field)
                { return field.widget == child.getReference() && field.field == source.field; });
            if (found == fields.end() || !encountered.insert(found->widget).second ||
                ++widgetCount > 1000)
                fail("フォームのwidget参照または上限が不正です。");
            const auto widget = document.getObject(child);
            const auto widgetId = metadata(widget, "TatsujinWidget")["id"].toString();
            if (entry.widgets.isEmpty())
            {
                entry.kind = found->kind;
                entry.values = found->values;
                entry.exports = found->exports;
                entry.labels = found->labels;
            }
            else if (entry.kind != found->kind || entry.values != found->values)
                fail("フォームの共有値または種類が不整合です。");
            if (found->kind == FormKind::Checkbox || found->kind == FormKind::Radio)
            {
                entry.exports << found->onState;
                entry.labels << loader.readTextString(widget.getDictionary()->get("TU"), {});
            }
            entry.widgets << FormDesignWidget{
                widgetId, found->page,
                pageMatrix(document.getCatalog()->getPage(found->page)).mapRect(found->rectangle)};
        }
        if (entry.widgets.isEmpty() || entry.kind == FormKind::Unsupported ||
            metadata(object, "TatsujinForm")["kind"].toInt(-1) != int(entry.kind))
            fail("再設計できないフォームの構造です。");
        result << entry;
    }
    // Detect an owned widget hidden outside its canonical field tree or repeated
    // on another page; never silently orphan or repair it while designing.
    std::set<PDFObjectReference> inPages;
    for (int page = 0; page < int(document.getCatalog()->getPageCount()); ++page)
    {
        stop(cancelled);
        const auto object =
            document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
        for (const auto& annotation : array(document, object.getDictionary()->get("Annots")))
        {
            const auto widget = document.getObject(annotation);
            if (metadata(widget, "TatsujinWidget").isEmpty())
                continue;
            if (!annotation.isReference() || !encountered.contains(annotation.getReference()) ||
                !inPages.insert(annotation.getReference()).second)
                fail("フォームのwidgetが孤立または重複しています。");
        }
    }
    if (inPages != encountered)
        fail("フォームのwidgetがページから見つかりません。");
    return result;
}
PDFDocument replaceFormDesign(const PDFDocument& document, const QVector<FormDesignEntry>& entries,
                              const std::function<bool()>& cancelled,
                              const std::function<void(int, int)>& progress)
{
    valid(document);
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    stop(cancelled);
    const auto originalEntries = formDesign(document, cancelled);
    const auto originals = existing(document, cancelled);
    QSet<QString> names, ids;
    const auto parsedForm = PDFForm::parse(&document, document.getCatalog()->getFormObject());
    int nodes = 0;
    std::function<void(const PDFFormFields&, int)> collect =
        [&](const PDFFormFields& fields, int depth)
    {
        if (depth > 32)
            fail("既存フォームの階層が大きすぎます。");
        for (const auto& field : fields)
        {
            stop(cancelled);
            if (++nodes > 100000)
                fail("既存フォームの構造が大きすぎます。");
            if (owned(document, PDFObject::createReference(field->getSelfReference())))
                continue;
            names.insert(field->getName(PDFFormField::FullyQualified));
            collect(field->getChildFields(), depth + 1);
        }
    };
    collect(parsedForm.getFormFields(), 0);
    qsizetype bytes = 0;
    int widgets = 0;
    for (const auto& entry : entries)
    {
        stop(cancelled);
        if (!idValid(entry.id) || ids.contains(entry.id))
            fail("フォームのIDが重複または不正です。");
        ids.insert(entry.id);
        textValid(entry.name, 256, false);
        textValid(entry.caption, 1024, false);
        if (entry.name.isEmpty() || entry.name != entry.name.trimmed() ||
            entry.name.contains('.') || names.contains(entry.name))
            fail("内部名は重複・階層区切り・前後の空白を避けて指定してください。");
        names.insert(entry.name);
        if (entry.kind < FormKind::Text || entry.kind > FormKind::List || entry.widgets.isEmpty() ||
            (entry.kind != FormKind::Radio && entry.widgets.size() != 1) ||
            entry.exports.size() != entry.labels.size() || entry.exports.size() > 1000 ||
            entry.maxLength < 0 || entry.maxLength > 65536 ||
            (entry.maxLength && entry.kind != FormKind::Text &&
             entry.kind != FormKind::Multiline) ||
            (entry.multiple && entry.kind != FormKind::List) ||
            (entry.editableChoice && entry.kind != FormKind::Combo))
            fail("フォームの種類・選択肢・設定が不正です。");
        const bool button = entry.kind == FormKind::Checkbox || entry.kind == FormKind::Radio;
        const bool choice = entry.kind == FormKind::Combo || entry.kind == FormKind::List;
        if ((button && entry.exports.size() != entry.widgets.size()) ||
            (choice && entry.exports.isEmpty()) || (!button && !choice && !entry.exports.isEmpty()))
            fail("フォームの選択肢が足りないか、この種類には使えません。");
        QSet<QString> options, values;
        for (int i = 0; i < entry.exports.size(); ++i)
        {
            const auto value = entry.exports[i];
            textValid(value, 65536, false);
            textValid(entry.labels[i], 65536, false);
            validateFormFontText(entry.labels[i]);
            if (value.isEmpty() || options.contains(value) || (button && value == "Off"))
                fail("選択値の重複・空欄・予約値があります。");
            options.insert(value);
            bytes += value.toUtf8().size() + entry.labels[i].toUtf8().size();
        }
        if (entry.values.size() > 1 && !(entry.kind == FormKind::List && entry.multiple))
            fail("この項目は複数の初期値に対応していません。");
        for (const auto& value : entry.values)
        {
            textValid(value, 65536, entry.kind == FormKind::Multiline);
            if (!button)
                validateFormFontText(value);
            if (values.contains(value) || (button && value != "Off" && !options.contains(value)) ||
                (choice && !entry.editableChoice && !options.contains(value) &&
                 !(entry.kind == FormKind::Combo && value.isEmpty())) ||
                (entry.maxLength && value.toUcs4().size() > entry.maxLength))
                fail("フォームの初期値が選択肢または最大文字数に合いません。");
            values.insert(value);
            bytes += value.toUtf8().size();
        }
        bytes += entry.name.toUtf8().size() + entry.caption.toUtf8().size() + 512;
        for (const auto& widget : entry.widgets)
        {
            if (++widgets > 1000 || !idValid(widget.id) || ids.contains(widget.id) ||
                widget.page < 0 || widget.page >= int(document.getCatalog()->getPageCount()))
                fail("フォームのwidget数・ID・配置ページが不正です。");
            ids.insert(widget.id);
            const auto box = widget.rectangle;
            const auto size = pageSize(document.getCatalog()->getPage(widget.page));
            for (double v : {box.x(), box.y(), box.width(), box.height()})
                if (!std::isfinite(v))
                    fail("フォームの座標が不正です。");
            if (box.width() < 6 || box.height() < 6 || box.x() < -1e-8 || box.y() < -1e-8 ||
                box.right() > size.width() + 1e-8 || box.bottom() > size.height() + 1e-8)
                fail("フォームの範囲は用紙内で幅・高さ6pt以上にしてください。");
            bytes += 256;
        }
        if (bytes > 16 * 1024 * 1024)
            fail("フォームの設計データが16MiBを超えています。");
    }
    if (entries == originalEntries)
        return document;
    PDFDocumentBuilder builder(&document);
    auto form = formDictionary(document);
    auto fontObject = form.get("TatsujinFormFont");
    PDFObjectReference font;
    if (!entries.isEmpty())
    {
        if (fontObject.isReference() && isFormFont(document, fontObject.getReference()))
            font = fontObject.getReference();
        else
            font = embedFormFont(builder);
        set(form, "TatsujinFormFont", PDFObject::createReference(font));
    }
    auto resourcesObject = document.getObject(form.get("DR"));
    PDFDictionary resources =
        resourcesObject.isDictionary() ? *resourcesObject.getDictionary() : PDFDictionary{};
    auto fontsObject = document.getObject(resources.get("Font"));
    PDFDictionary fonts =
        fontsObject.isDictionary() ? *fontsObject.getDictionary() : PDFDictionary{};
    QByteArray fontName = "TatsuJP";
    for (int index = 1;
         fonts.hasKey(fontName) && fonts.get(fontName) != PDFObject::createReference(font); ++index)
        fontName = "TatsuJP" + QByteArray::number(index);
    if (!entries.isEmpty())
    {
        set(fonts, fontName.constData(), PDFObject::createReference(font));
        set(resources, "Font", dictObject(fonts));
        set(form, "DR", dictObject(resources));
    }
    std::vector<PDFObject> rootFields;
    std::set<PDFObjectReference> removedWidgets;
    for (const auto& [id, source] : originals)
        for (const auto& [widgetId, reference] : source.widgets)
            removedWidgets.insert(reference);
    for (const auto& field : array(document, form.get("Fields")))
        if (!owned(document, field))
            rootFields.push_back(field);
    std::vector<std::vector<PDFObject>> annotations(document.getCatalog()->getPageCount());
    for (int page = 0; page < int(annotations.size()); ++page)
    {
        stop(cancelled);
        const auto object =
            document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
        for (const auto& annotation : array(document, object.getDictionary()->get("Annots")))
            if (!annotation.isReference() || !removedWidgets.contains(annotation.getReference()))
                annotations[page].push_back(annotation);
    }
    int completed = 0;
    for (const auto& entry : entries)
    {
        stop(cancelled);
        const auto found = originals.find(entry.id);
        const auto fieldRef =
            found == originals.end() ? builder.addObject(dictObject({})) : found->second.field;
        auto field = found == originals.end()
                         ? PDFDictionary{}
                         : *document.getObjectByReference(fieldRef).getDictionary();
        for (auto key :
             {"FT", "Ff", "T", "TU", "V", "DV", "DA", "Opt", "I", "TI", "MaxLen", "Kids"})
            field.removeEntry(key);
        const bool button = entry.kind == FormKind::Checkbox || entry.kind == FormKind::Radio;
        const bool choice = entry.kind == FormKind::Combo || entry.kind == FormKind::List;
        set(field, "FT", PDFObject::createName(button ? "Btn" : choice ? "Ch" : "Tx"));
        int flags = (entry.readOnly ? 1 : 0) | (entry.required ? 2 : 0) |
                    (entry.kind == FormKind::Multiline ? 1 << 12 : 0) |
                    (entry.kind == FormKind::Radio ? 1 << 15 : 0) |
                    (entry.kind == FormKind::Combo ? 1 << 17 : 0) |
                    (entry.editableChoice ? 1 << 18 : 0) | (entry.multiple ? 1 << 21 : 0);
        set(field, "Ff", PDFObject::createInteger(flags));
        set(field, "T", PDFObjectFactory::createTextString(entry.name));
        set(field, "TU", PDFObjectFactory::createTextString(entry.caption));
        set(field, "V", valueObject(entry));
        set(field, "DV", valueObject(entry));
        if (entry.maxLength && !button && !choice)
            set(field, "MaxLen", PDFObject::createInteger(entry.maxLength));
        if (choice)
        {
            std::vector<PDFObject> options, indices;
            for (int i = 0; i < entry.exports.size(); ++i)
            {
                options.push_back(arrObject({PDFObjectFactory::createTextString(entry.exports[i]),
                                             PDFObjectFactory::createTextString(entry.labels[i])}));
                if (entry.values.contains(entry.exports[i]))
                    indices.push_back(PDFObject::createInteger(i));
            }
            set(field, "Opt", arrObject(std::move(options)));
            if (entry.kind == FormKind::List)
            {
                set(field, "I", arrObject(indices));
                set(field, "TI", indices.empty() ? PDFObject::createInteger(0) : indices.front());
            }
        }
        set(field, "TatsujinForm",
            PDFObject::createString(
                QJsonDocument(
                    QJsonObject{{"version", 1}, {"id", entry.id}, {"kind", int(entry.kind)}})
                    .toJson(QJsonDocument::Compact)));
        std::vector<PDFObject> kids;
        for (int index = 0; index < entry.widgets.size(); ++index)
        {
            stop(cancelled);
            const auto& placement = entry.widgets[index];
            const auto page = document.getCatalog()->getPage(placement.page);
            const double unit = page->getUserUnit();
            const auto box = pageMatrix(page).inverted().mapRect(placement.rectangle);
            const auto size = placement.rectangle.size() / unit;
            const int rotation = int(page->getPageRotation()) * 90;
            PDFObjectReference reference;
            if (found != originals.end())
                if (const auto old = found->second.widgets.find(placement.id);
                    old != found->second.widgets.end())
                    reference = old->second;
            auto widget = reference.isValid()
                              ? *document.getObjectByReference(reference).getDictionary()
                              : PDFDictionary{};
            if (!reference.isValid())
                reference = builder.addObject(dictObject({}));
            for (auto key : {"AP", "AS", "Rect", "P", "Parent", "DA", "MK", "TU"})
                widget.removeEntry(key);
            set(widget, "Type", PDFObject::createName("Annot"));
            set(widget, "Subtype", PDFObject::createName("Widget"));
            set(widget, "Parent", PDFObject::createReference(fieldRef));
            set(widget, "P", PDFObject::createReference(page->getPageReference()));
            set(widget, "Rect", rectObject(box));
            set(widget, "F", PDFObject::createInteger(4));
            set(widget, "TU",
                PDFObjectFactory::createTextString(button ? entry.labels[index] : entry.caption));
            set(widget, "TatsujinWidget",
                PDFObject::createString(
                    QJsonDocument(
                        QJsonObject{{"version", 1}, {"id", placement.id}, {"field", entry.id}})
                        .toJson(QJsonDocument::Compact)));
            PDFDictionary appearance, characteristics;
            set(characteristics, "R", PDFObject::createInteger(rotation));
            set(characteristics, "BC", arrObject({number(.4)}));
            set(characteristics, "BG", arrObject({number(1)}));
            set(widget, "MK", dictObject(characteristics));
            if (button)
            {
                const auto state = entry.exports[index].toUtf8();
                PDFDictionary normal;
                set(normal, "Off",
                    PDFObject::createReference(buttonAppearance(
                        builder, size, false, entry.kind == FormKind::Radio, rotation)));
                set(normal, state.constData(),
                    PDFObject::createReference(buttonAppearance(
                        builder, size, true, entry.kind == FormKind::Radio, rotation)));
                set(appearance, "N", dictObject(normal));
                set(widget, "AS",
                    PDFObject::createName(
                        entry.values.value(0) == entry.exports[index] ? state : QByteArray("Off")));
            }
            else
            {
                FormField values;
                values.kind = entry.kind;
                values.exports = entry.exports;
                values.labels = entry.labels;
                set(appearance, "N",
                    PDFObject::createReference(formFontAppearance(builder, font, size, values,
                                                                  entry.values, unit, rotation)));
                set(widget, "TatsujinFormFont", PDFObject::createReference(font));
                set(widget, "DA",
                    PDFObjectFactory::createTextString(QString::fromLatin1(
                        '/' + fontName + ' ' + QByteArray::number(12 / unit, 'f', 8) + " Tf 0 g")));
            }
            set(widget, "AP", dictObject(appearance));
            builder.setObject(reference, dictObject(widget));
            kids.push_back(PDFObject::createReference(reference));
            annotations[placement.page].push_back(PDFObject::createReference(reference));
        }
        set(field, "Kids", arrObject(std::move(kids)));
        if (!button)
            set(field, "DA",
                PDFObjectFactory::createTextString(
                    QString::fromLatin1('/' + fontName + " 12 Tf 0 g")));
        builder.setObject(fieldRef, dictObject(field));
        rootFields.push_back(PDFObject::createReference(fieldRef));
        if (progress)
            progress(++completed, entries.size());
        stop(cancelled);
    }
    set(form, "Fields", arrObject(std::move(rootFields)));
    set(form, "NeedAppearances", PDFObject::createBool(false));
    const auto formRef = document.getCatalog()->getFormObject();
    if (formRef.isReference())
        builder.setObject(formRef.getReference(), dictObject(form));
    else
        builder.setCatalogAcroForm(builder.addObject(dictObject(form)));
    for (int page = 0; page < int(annotations.size()); ++page)
    {
        stop(cancelled);
        const auto reference = document.getCatalog()->getPage(page)->getPageReference();
        auto dictionary = *builder.getObjectByReference(reference).getDictionary();
        set(dictionary, "Annots", arrObject(std::move(annotations[page])));
        builder.setObject(reference, dictObject(dictionary));
    }
    stop(cancelled);
    auto candidate = builder.build();
    // Verify the actual field tree and page annotations before returning the
    // private snapshot; no partial document is committed on failure.
    const auto verified = formDesign(candidate, cancelled);
    if (verified.size() != entries.size())
        fail("フォーム設計の保存候補を確認できません。");
    for (const auto& entry : entries)
    {
        auto expected = entry;
        if (expected.values.isEmpty() && expected.kind != FormKind::List)
            expected.values << (expected.kind == FormKind::Checkbox ||
                                        expected.kind == FormKind::Radio
                                    ? "Off"
                                    : "");
        const auto actual = std::find_if(verified.begin(), verified.end(), [&](const auto& value)
                                         { return value.id == expected.id; });
        if (actual == verified.end())
            fail("フォーム設計の項目を保存候補から確認できません: " + expected.name);
        QStringList differences;
        if (actual->name != expected.name || actual->caption != expected.caption)
            differences << "名前・表示ラベル";
        if (actual->kind != expected.kind)
            differences << "種類";
        if (actual->values != expected.values)
            differences << "値";
        if (actual->exports != expected.exports || actual->labels != expected.labels)
            differences << "選択肢";
        if (actual->widgets != expected.widgets)
            differences << "widget・座標";
        if (actual->readOnly != expected.readOnly || actual->required != expected.required ||
            actual->multiple != expected.multiple ||
            actual->editableChoice != expected.editableChoice ||
            actual->maxLength != expected.maxLength)
            differences << "入力設定";
        if (!differences.isEmpty())
            fail("フォーム設計の保存候補に不整合があります: " + expected.name + "（" +
                 differences.join("・") + "）。元の文書は変更していません。");
    }
    stop(cancelled);
    return candidate;
}
} // namespace tatsu
