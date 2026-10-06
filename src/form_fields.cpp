#include "form_fields.h"
#include "pdf_appearance.h"
#include "pdf_objects.h"
#include "pdfform.h"
#include <QTextDocument>

namespace tatsu
{
using namespace detail;
QVector<FormField> formFields(const PDFDocument& document)
{
    auto form = PDFForm::parse(&document, document.getCatalog()->getFormObject());
    if (!form.isAcroForm())
        return {};
    PDFDocumentDataLoaderDecorator loader(&document);
    QVector<FormField> result;
    for (int page = 0; page < int(document.getCatalog()->getPageCount()); ++page)
    {
        auto p =
            document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
        auto annotations = document.getObject(p.getDictionary()->get("Annots"));
        if (!annotations.isArray())
            continue;
        for (const auto& annotation : *annotations.getArray())
        {
            if (!annotation.isReference())
                continue;
            auto field = form.getFormFieldForWidget(annotation.getReference());
            if (!field)
                continue;
            auto object = document.getObject(annotation);
            if (!object.isDictionary())
                continue;
            FormField item;
            const PDFFormField* owner = field;
            while (owner->getParentField() &&
                   owner->getParentField()->getName(PDFFormField::FullyQualified) ==
                       owner->getName(PDFFormField::FullyQualified))
                owner = owner->getParentField();
            item.field = owner->getSelfReference();
            item.widget = annotation.getReference();
            item.page = page;
            item.rectangle = loader.readRectangle(object.getDictionary()->get("Rect"), {});
            item.name = field->getName(PDFFormField::UserCaption);
            if (item.name.isEmpty())
                item.name = field->getName(PDFFormField::FullyQualified);
            item.readOnly = field->getFlags().testFlag(PDFFormField::ReadOnly);
            auto value = document.getObject(field->getValue());
            if (value.isName())
                item.values.append(QString::fromLatin1(value.getString()));
            else if (value.isString())
                item.values.append(loader.readTextString(value, {}));
            else if (value.isArray())
                for (const auto& part : *value.getArray())
                    item.values.append(loader.readTextString(part, {}));
            const auto flags = field->getFlags();
            item.noToggleOff = flags.testFlag(PDFFormField::NoToggleToOff);
            if (field->getFieldType() == PDFFormField::FieldType::Text)
            {
                auto text = static_cast<const PDFFormFieldText*>(field);
                item.kind =
                    flags.testFlag(PDFFormField::Multiline) ? FormKind::Multiline : FormKind::Text;
                item.maxLength = int(text->getTextMaximalLength());
                if (flags.testFlag(PDFFormField::Password) ||
                    flags.testFlag(PDFFormField::RichText))
                {
                    item.kind = FormKind::Unsupported;
                    item.notice = "パスワード／リッチテキストのフォーム入力は非対応です。";
                }
            }
            else if (field->getFieldType() == PDFFormField::FieldType::Button)
            {
                if (flags.testFlag(PDFFormField::PushButton))
                    item.notice = "送信などのフォームボタンは実行しません。";
                else
                {
                    item.kind =
                        flags.testFlag(PDFFormField::Radio) ? FormKind::Radio : FormKind::Checkbox;
                    auto appearances = document.getObject(object.getDictionary()->get("AP"));
                    if (appearances.isDictionary())
                    {
                        auto normal = document.getObject(appearances.getDictionary()->get("N"));
                        if (normal.isDictionary())
                            for (size_t i = 0; i < normal.getDictionary()->getCount(); ++i)
                            {
                                auto state = normal.getDictionary()->getKey(i).getString();
                                if (state != "Off")
                                {
                                    item.onState = QString::fromLatin1(state);
                                    break;
                                }
                            }
                    }
                    if (item.onState.isEmpty())
                    {
                        item.kind = FormKind::Unsupported;
                        item.notice = "チェック状態の外観がないフォームは非対応です。";
                    }
                }
            }
            else if (field->getFieldType() == PDFFormField::FieldType::Choice)
            {
                auto choice = static_cast<const PDFFormFieldChoice*>(field);
                item.kind = choice->isComboBox() ? FormKind::Combo : FormKind::List;
                item.editableChoice = choice->isEditableComboBox();
                item.multiple = flags.testFlag(PDFFormField::MultiSelect);
                for (const auto& option : choice->getOptions())
                {
                    item.exports.append(option.exportString);
                    item.labels.append(option.userString);
                }
            }
            else
                item.notice = "この種類のフォーム入力は非対応です。";
            if (item.values.isEmpty())
            {
                if (item.kind == FormKind::Checkbox || item.kind == FormKind::Radio)
                    item.values.append("Off");
                else if (item.kind != FormKind::List && item.kind != FormKind::Unsupported)
                    item.values.append("");
            }
            result.append(std::move(item));
        }
    }
    return result;
}
namespace
{
PDFObject textObject(const QString& text)
{
    PDFObjectFactory factory;
    factory << text;
    return factory.takeObject();
}
QColor widgetColor(const PDFDocument& document, const PDFDictionary* dictionary, const char* key,
                   QColor fallback)
{
    auto characteristics = document.getObject(dictionary->get("MK"));
    if (!characteristics.isDictionary())
        return fallback;
    PDFDocumentDataLoaderDecorator loader(&document);
    auto color = loader.readNumberArrayFromDictionary(characteristics.getDictionary(), key);
    if (color.size() == 1)
        return QColor::fromRgbF(color[0], color[0], color[0]);
    if (color.size() == 3)
        return QColor::fromRgbF(color[0], color[1], color[2]);
    if (color.size() == 4)
        return QColor::fromCmykF(color[0], color[1], color[2], color[3]);
    return fallback;
}
PDFObjectReference fieldAppearance(PDFDocumentBuilder& builder, const PDFDocument& document,
                                   const FormField& field, const QStringList& values)
{
    const auto widget = document.getObjectByReference(field.widget);
    const auto dimensions = field.rectangle.size();
    const double unit = document.getCatalog()->getPage(field.page)->getUserUnit();
    const double padding = qMin(2.0 / unit, dimensions.height() / 8);
    QFont font(signatureFont());
    font.setPixelSize(100);
    font.setStyleStrategy(QFont::NoFontMerging);
    auto raw = QRawFont::fromFont(font);
    const auto background = widgetColor(document, widget.getDictionary(), "BG", Qt::white);
    const auto border = widgetColor(document, widget.getDictionary(), "BC", QColor("#65768c"));
    QString text;
    if (field.kind == FormKind::Combo)
    {
        int index = field.exports.indexOf(values.value(0));
        text = index >= 0 ? field.labels[index] : values.value(0);
    }
    else if (field.kind != FormKind::List)
        text = values.value(0);
    QTextDocument layout;
    layout.setDocumentMargin(0);
    layout.setDefaultFont(font);
    auto option = layout.defaultTextOption();
    option.setWrapMode(field.kind == FormKind::Multiline ? QTextOption::WrapAtWordBoundaryOrAnywhere
                                                         : QTextOption::NoWrap);
    layout.setDefaultTextOption(option);
    layout.setPlainText(text);
    double scale = 12.0 / (100 * unit);
    if (field.kind != FormKind::List)
    {
        for (;;)
        {
            layout.setTextWidth((dimensions.width() - 2 * padding) / scale);
            if (layout.size().height() * scale <= dimensions.height() - 2 * padding &&
                layout.idealWidth() * scale <= dimensions.width() - 2 * padding)
                break;
            scale *= .92;
            if (scale < 6.0 / (100 * unit))
                fail("入力がフォーム欄に収まりません。内容を短くしてください。未保存の文書は保持し"
                     "ています。");
        }
    }
    const auto allText = field.kind == FormKind::List ? field.labels.join('\n') : text;
    return painterAppearance(
        builder, dimensions,
        [&](QPainter* painter)
        {
            painter->fillRect(QRectF(QPointF(), dimensions), background);
            painter->setPen(QPen(border, .8 / unit));
            painter->drawRect(QRectF(QPointF(), dimensions)
                                  .adjusted(.4 / unit, .4 / unit, -.4 / unit, -.4 / unit));
            painter->translate(padding, padding);
            painter->scale(scale, scale);
            if (field.kind != FormKind::List)
                layout.drawContents(painter);
            else
            {
                painter->setFont(font);
                QFontMetricsF metrics(font);
                int start = field.exports.indexOf(values.value(0));
                start = qMax(0, start);
                const double width = (dimensions.width() - 2 * padding) / scale;
                for (int i = start; i < field.labels.size(); ++i)
                {
                    double top = (i - start) * metrics.lineSpacing();
                    if ((top + metrics.lineSpacing()) * scale > dimensions.height() - 2 * padding)
                        break;
                    const bool selected = values.contains(field.exports[i]);
                    if (selected)
                        painter->fillRect(QRectF(0, top, width, metrics.lineSpacing()),
                                          QColor("#185fc3"));
                    painter->setPen(selected ? Qt::white : Qt::black);
                    painter->drawText(QPointF(0, top + metrics.ascent()), field.labels[i]);
                }
            }
        },
        allText, raw);
}
} // namespace
void putFormValue(Document& document, PDFObjectReference widget, const QStringList& values)
{
    document.editable();
    auto fields = formFields(document.pdf());
    auto it = std::find_if(fields.begin(), fields.end(),
                           [&](const auto& field) { return field.widget == widget; });
    if (it == fields.end() || it->readOnly || it->kind == FormKind::Unsupported)
        fail("このフォームには入力できません。");
    const auto selected = *it;
    const bool button = selected.kind == FormKind::Checkbox || selected.kind == FormKind::Radio;
    if (selected.kind == FormKind::Radio && selected.noToggleOff && values.value(0) == "Off" &&
        selected.values.value(0) != "Off")
        fail("このラジオボタンは選択解除できません。別の選択肢を選んでください。");
    if (button && values.value(0) != selected.onState && values.value(0) != "Off")
        fail("チェック状態が不正です。");
    if (!button && selected.kind != FormKind::List && values.size() > 1)
        fail("この項目は複数値に対応していません。");
    if (selected.kind == FormKind::Text &&
        (values.value(0).contains('\n') || values.value(0).contains('\r')))
        fail("単行フォームには改行を入力できません。");
    if (selected.maxLength > 0 && values.value(0).toUcs4().size() > selected.maxLength)
        fail("フォームの最大文字数を超えています。");
    if (selected.kind == FormKind::Combo || selected.kind == FormKind::List)
    {
        if (!selected.multiple && values.size() > 1)
            fail("この選択欄は複数選択に対応していません。");
        QSet<QString> seen;
        for (const auto& value : values)
        {
            if ((!selected.editableChoice && !selected.exports.contains(value)) ||
                seen.contains(value))
                fail("選択値が不正です。");
            seen.insert(value);
        }
    }
    if (selected.values == values)
        return;
    PDFDocumentBuilder builder(&document.pdf());
    PDFObject value;
    if (button)
        value = PDFObject::createName(values.value(0, "Off").toLatin1());
    else if (values.size() <= 1)
        value = textObject(values.value(0));
    else
    {
        std::vector<PDFObject> parts;
        for (const auto& text : values)
            parts.push_back(textObject(text));
        value = arrObject(parts);
    }
    builder.setFormFieldValue(selected.field, value);
    if (selected.kind == FormKind::List)
    {
        std::vector<PDFInteger> indices;
        for (const auto& text : values)
            indices.push_back(selected.exports.indexOf(text));
        std::sort(indices.begin(), indices.end());
        builder.setFormFieldChoiceIndices(selected.field, indices);
        builder.setFormFieldChoiceTopIndex(selected.field, indices.empty() ? 0 : indices.front());
    }
    for (const auto& field : fields)
    {
        if (field.field != selected.field)
            continue;
        auto dictionary = *builder.getObjectByReference(field.widget).getDictionary();
        if (button)
            set(dictionary, "AS",
                PDFObject::createName(values.value(0) == field.onState ? field.onState.toLatin1()
                                                                       : QByteArray("Off")));
        else
        {
            const auto reference = fieldAppearance(builder, document.pdf(), field, values);
            auto appearanceObject = document.pdf().getObject(dictionary.get("AP"));
            PDFDictionary appearance = appearanceObject.isDictionary()
                                           ? *appearanceObject.getDictionary()
                                           : PDFDictionary();
            set(appearance, "N", PDFObject::createReference(reference));
            set(dictionary, "AP", dictObject(appearance));
        }
        builder.setObject(field.widget, dictObject(dictionary));
    }
    auto formObject = document.pdf().getObject(document.pdf().getCatalog()->getFormObject());
    if (formObject.isDictionary())
    {
        auto form = *formObject.getDictionary();
        set(form, "NeedAppearances", PDFObject::createBool(false));
        const auto reference = document.pdf().getCatalog()->getFormObject();
        if (reference.isReference())
            builder.setObject(reference.getReference(), dictObject(form));
        else
            builder.setCatalogAcroForm(builder.addObject(dictObject(form)));
    }
    document.commit(builder.build());
}
} // namespace tatsu
