#include "page_decoration.h"
#include "pdf_appearance.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "text_font.h"
#include <cmath>

namespace tatsu
{
using namespace detail;
namespace
{
constexpr auto metadataKey = "TatsujinDecoration";
void validDocument(const PDFDocument& document)
{
    if (!document.getCatalog() || !document.getCatalog()->getPageCount() ||
        !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
}
void editableDocument(const PDFDocument& document)
{
    validDocument(document);
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
}
void checkCancel(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("処理を中止しました。文書は変更していません。");
}
QJsonObject settings(const DecorationOptions& options)
{
    return {{"kind", int(options.kind)},
            {"header", QJsonArray::fromStringList(options.header)},
            {"footer", QJsonArray::fromStringList(options.footer)},
            {"watermark", options.watermark},
            {"fontFamily", options.fontFamily},
            {"size", options.size},
            {"color", options.color.name()},
            {"left", options.margins.left()},
            {"top", options.margins.top()},
            {"right", options.margins.right()},
            {"bottom", options.margins.bottom()},
            {"startNumber", options.startNumber},
            {"angle", options.angle},
            {"opacity", options.opacity}};
}
DecorationOptions readSettings(const QJsonObject& object)
{
    for (const auto& key :
         {"kind", "size", "left", "top", "right", "bottom", "startNumber", "angle", "opacity"})
        if (!object[key].isDouble())
            fail("保存されたページ装飾の設定が不正です。変更していません。");
    for (const auto& key : {"watermark", "fontFamily", "color"})
        if (!object[key].isString())
            fail("保存されたページ装飾の設定が不正です。変更していません。");
    DecorationOptions options;
    options.kind = DecorationKind(object["kind"].toInt(-1));
    auto lines = [&](const char* key)
    {
        const auto array = object[key].toArray();
        if (array.size() != 3)
            fail("保存されたヘッダー／フッターの設定が不正です。");
        QStringList result;
        for (const auto& value : array)
        {
            if (!value.isString())
                fail("保存されたヘッダー／フッターの設定が不正です。");
            result << value.toString();
        }
        return result;
    };
    options.header = lines("header");
    options.footer = lines("footer");
    options.watermark = object["watermark"].toString();
    options.fontFamily = object["fontFamily"].toString();
    options.size = object["size"].toDouble();
    options.color = QColor(object["color"].toString());
    options.margins = {object["left"].toDouble(), object["top"].toDouble(),
                       object["right"].toDouble(), object["bottom"].toDouble()};
    options.startNumber = object["startNumber"].toInt();
    options.angle = object["angle"].toDouble();
    options.opacity = object["opacity"].toDouble();
    if (settings(options) != object)
        fail("保存されたページ装飾に未対応の設定があります。変更していません。");
    if ((options.kind != DecorationKind::HeaderFooter &&
         options.kind != DecorationKind::Watermark) ||
        options.size < 6 || options.size > 144 || !options.color.isValid() ||
        options.startNumber < 1 || options.startNumber > 999999 || options.angle < -90 ||
        options.angle > 90 || options.opacity < .05 || options.opacity > 1)
        fail("保存されたページ装飾の値が不正です。変更していません。");
    for (double value : {options.margins.left(), options.margins.top(), options.margins.right(),
                         options.margins.bottom()})
        if (value < 0 || value > 100)
            fail("保存されたページ装飾の余白が不正です。変更していません。");
    return options;
}
struct Owned
{
    int page;
    PDFObjectReference reference;
    QString id;
    DecorationOptions options;
};
QVector<Owned> owned(const PDFDocument& document)
{
    validDocument(document);
    QVector<Owned> result;
    for (int page = 0; page < int(document.getCatalog()->getPageCount()); ++page)
    {
        const auto object =
            document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
        const auto annotations = document.getObject(object.getDictionary()->get("Annots"));
        if (!annotations.isArray())
            continue;
        for (const auto& reference : *annotations.getArray())
        {
            const auto annotation = document.getObject(reference);
            if (!annotation.isDictionary() || !annotation.getDictionary()->hasKey(metadataKey))
                continue;
            const auto value = document.getObject(annotation.getDictionary()->get(metadataKey));
            const auto meta = value.isString() ? QJsonDocument::fromJson(value.getString()).object()
                                               : QJsonObject{};
            const auto id = meta["group"].toString();
            if (!reference.isReference() || meta["version"].toInt(-1) != 1 || QUuid(id).isNull() ||
                !meta["settings"].isObject())
                fail("保存されたページ装飾を編集できません。未知の設定は削除しません。");
            result << Owned{page, reference.getReference(), id,
                            readSettings(meta["settings"].toObject())};
        }
    }
    return result;
}
QString expanded(QString text, int page, int count, int start)
{
    if (text.size() > 200 || text.contains('\n') || text.contains('\r') || text.contains('\t'))
        fail("各欄は改行・タブのない200文字以内で指定してください。");
    text.replace("{page}", QString::number(qint64(start) + page));
    text.replace("{pages}", QString::number(count));
    if (text.contains('{') || text.contains('}'))
        fail("ページ番号は {page}、総ページ数は {pages} を使ってください。");
    return text;
}
struct Line
{
    QString text;
    QRectF box;
    double inkLeft = 0;
};
struct Plan
{
    int page;
    QVector<Line> lines;
    QString text;
};
void exactOpacity(PDFDocumentBuilder& builder, PDFObjectReference appearance, double opacity)
{
    if (opacity == 1)
        return;
    const auto form = builder.getObjectByReference(appearance);
    const auto resources = builder.getObject(form.getStream()->getDictionary()->get("Resources"));
    const auto states = builder.getObject(resources.getDictionary()->get("ExtGState"));
    bool corrected = false;
    if (states.isDictionary())
        for (size_t i = 0; i < states.getDictionary()->getCount(); ++i)
        {
            const auto reference = states.getDictionary()->getValue(i);
            const auto object = builder.getObject(reference);
            if (!reference.isReference() || !object.isDictionary())
                continue;
            auto state = *object.getDictionary();
            const auto alpha = builder.getObject(state.get("ca"));
            if (!alpha.isReal() || alpha.getReal() >= 1)
                continue;
            // QPdfWriter quantizes painter opacity to an 8-bit alpha. These
            // resources were privately copied for this AP; preserve the user's
            // exact PDF value without changing another annotation's resources.
            if (qAbs(alpha.getReal() - qRound(opacity * 255) / 255.0) > .000001)
                fail("透かしの不透明度を正しく保存できません。文書は変更していません。");
            set(state, "ca", number(opacity));
            set(state, "CA", number(opacity));
            builder.setObject(reference.getReference(), dictObject(state));
            corrected = true;
        }
    if (!corrected)
        fail("透かしの不透明度を保存できません。文書は変更していません。");
}
QVector<Plan> plans(const PDFDocument& document, const QVector<int>& pages,
                    const DecorationOptions& options, const std::function<bool()>& cancelled)
{
    editableDocument(document);
    if (pages.isEmpty() || pages.size() > 1000 || options.header.size() != 3 ||
        options.footer.size() != 3 ||
        (options.kind != DecorationKind::HeaderFooter && options.kind != DecorationKind::Watermark))
        fail("対象は1〜1000ページです。設定を確認してください。");
    if (!std::isfinite(options.size) || options.size < 6 || options.size > 144 ||
        !options.color.isValid() || options.color.alpha() != 255 || options.startNumber < 1 ||
        options.startNumber > 999999 || !std::isfinite(options.angle) || options.angle < -90 ||
        options.angle > 90 || !std::isfinite(options.opacity) || options.opacity < .05 ||
        options.opacity > 1)
        fail("文字の大きさ・色・番号・角度・不透明度を確認してください。");
    for (double margin : {options.margins.left(), options.margins.top(), options.margins.right(),
                          options.margins.bottom()})
        if (!std::isfinite(margin) || margin < 0 || margin > 100)
            fail("四辺の距離は0〜100mmで指定してください。");
    const auto font = textFont(options.fontFamily.isEmpty() ? signatureFont() : options.fontFamily);
    const QRawFont raw = QRawFont::fromFont(font);
    const QFontMetricsF metrics(font);
    const double scale = options.size / 100.0;
    QSet<int> seen;
    QVector<Plan> result;
    const int count = int(document.getCatalog()->getPageCount());
    for (int page : pages)
    {
        checkCancel(cancelled);
        if (page < 0 || page >= count || seen.contains(page))
            fail("ページ範囲が不正か重複しています。");
        seen.insert(page);
        const auto physical = pageSize(document.getCatalog()->getPage(page));
        const double mm = 72.0 / 25.4;
        const QRectF available(
            options.margins.left() * mm, options.margins.top() * mm,
            physical.width() - (options.margins.left() + options.margins.right()) * mm,
            physical.height() - (options.margins.top() + options.margins.bottom()) * mm);
        if (!available.isValid())
            fail(QString("%1ページの余白が大きすぎます。").arg(page + 1));
        Plan plan{page, {}, {}};
        auto line = [&](const QString& input)
        {
            Line value;
            value.text = expanded(input, page, count, options.startNumber);
            for (auto cp : value.text.toUcs4())
                if (cp < 32 || !raw.supportsCharacter(cp))
                    fail(QString("対応していない文字があります: U+%1").arg(cp, 4, 16, QChar('0')));
            const auto ink = metrics.boundingRect(value.text);
            value.inkLeft = qMin(0.0, ink.left());
            value.box.setSize(
                {(qMax(metrics.horizontalAdvance(value.text), ink.right()) - value.inkLeft) *
                         scale +
                     2,
                 metrics.lineSpacing() * scale + 2});
            return value;
        };
        if (options.kind == DecorationKind::Watermark)
        {
            auto value = line(options.watermark);
            if (value.text.trimmed().isEmpty())
                fail("透かしの文字を入力してください。");
            value.box.moveCenter(available.center());
            QTransform rotation;
            rotation.translate(available.center().x(), available.center().y());
            rotation.rotate(options.angle);
            rotation.translate(-available.center().x(), -available.center().y());
            if (!available.contains(rotation.mapRect(value.box)))
                fail(QString(
                         "%1ページに透かしが収まりません。サイズ・角度・余白を変更してください。")
                         .arg(page + 1));
            plan.lines << value;
        }
        else
            for (int row = 0; row < 2; ++row)
                for (int column = 0; column < 3; ++column)
                {
                    const auto input = (row == 0 ? options.header : options.footer).at(column);
                    if (expanded(input, page, count, options.startNumber).trimmed().isEmpty())
                        continue;
                    auto value = line(input);
                    double x = available.left();
                    if (column == 1)
                        x = available.center().x() - value.box.width() / 2;
                    else if (column == 2)
                        x = available.right() - value.box.width();
                    const double y =
                        row == 0 ? available.top() : available.bottom() - value.box.height();
                    value.box.moveTopLeft({x, y});
                    if (!available.contains(value.box))
                        fail(QString(
                                 "%1ページに文字が収まりません。サイズ・余白を変更してください。")
                                 .arg(page + 1));
                    for (const auto& previous : plan.lines)
                        if (previous.box.intersects(value.box))
                            fail(QString("%1ページの欄が重なっています。文字・サイズを変更してくだ"
                                         "さい。")
                                     .arg(page + 1));
                    plan.lines << value;
                }
        if (plan.lines.isEmpty())
            fail("ヘッダーまたはフッターの文字を入力してください。");
        QStringList texts;
        for (const auto& value : plan.lines)
            texts << value.text;
        plan.text = texts.join('\n');
        result << plan;
    }
    return result;
}
PDFDocument build(const PDFDocument& document, const QVector<int>& pages, DecorationOptions options,
                  const QString& group, int previewPage, const std::function<bool()>& cancelled,
                  const std::function<void(int, int)>& progress)
{
    checkCancel(cancelled);
    if (options.fontFamily.isEmpty())
        options.fontFamily = signatureFont();
    const auto layout = plans(document, pages, options, cancelled);
    if (previewPage >= 0 && !pages.contains(previewPage))
        fail("対象のプレビューページを選んでください。");
    const auto groups = decorationGroups(document);
    if (!group.isEmpty())
    {
        const auto found = std::find_if(groups.begin(), groups.end(),
                                        [&](const auto& item) { return item.id == group; });
        if (found == groups.end() || found->options.kind != options.kind)
            fail("変更するページ装飾が見つかりません。");
    }
    const QString id = group.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : group;
    PDFDocumentBuilder builder(&document);
    for (const auto& item : owned(document))
        if (item.id == group)
        {
            checkCancel(cancelled);
            builder.removeAnnotation(document.getCatalog()->getPage(item.page)->getPageReference(),
                                     item.reference);
        }
    const auto font = textFont(options.fontFamily);
    const QRawFont raw = QRawFont::fromFont(font);
    const QFontMetricsF metrics(font);
    int completed = 0;
    if (progress)
        progress(0, layout.size());
    for (const auto& plan : layout)
    {
        checkCancel(cancelled);
        if (previewPage >= 0 && plan.page != previewPage)
            continue;
        const auto page = document.getCatalog()->getPage(plan.page);
        const auto box = page->getCropBox();
        const auto inverse = pageMatrix(page).inverted();
        auto local = [&](QPointF visual)
        {
            const auto pdfPoint = inverse.map(visual);
            return QPointF(pdfPoint.x() - box.left(), box.bottom() - pdfPoint.y());
        };
        const auto origin = local({0, 0}), x = local({1, 0}) - origin, y = local({0, 1}) - origin;
        const QTransform mapping(x.x(), x.y(), y.x(), y.y(), origin.x(), origin.y());
        const auto ap = painterAppearance(
            builder, box.size(),
            [&](QPainter* painter)
            {
                painter->setTransform(mapping);
                painter->setFont(font);
                painter->setPen(options.color);
                painter->setOpacity(options.kind == DecorationKind::Watermark ? options.opacity
                                                                              : 1);
                for (const auto& line : plan.lines)
                {
                    painter->save();
                    if (options.kind == DecorationKind::Watermark)
                    {
                        painter->translate(line.box.center());
                        painter->rotate(options.angle);
                        painter->translate(-line.box.center());
                    }
                    painter->translate(line.box.topLeft() + QPointF(1, 1));
                    painter->scale(options.size / 100.0, options.size / 100.0);
                    painter->drawText(QPointF(-line.inkLeft, metrics.ascent()), line.text);
                    painter->restore();
                }
            },
            plan.text, raw);
        PDFDictionary appearance;
        if (options.kind == DecorationKind::Watermark)
            exactOpacity(builder, ap, options.opacity);
        set(appearance, "N", PDFObject::createReference(ap));
        const auto reference = builder.createAnnotationStamp(
            page->getPageReference(), box, Stamp::Approved, "PDF達人",
            options.kind == DecorationKind::Watermark ? "透かし" : "ヘッダー／フッター", plan.text);
        auto annotation = *builder.getObjectByReference(reference).getDictionary();
        set(annotation, "Rect", rectObject(box));
        set(annotation, "AP", dictObject(appearance));
        set(annotation, "F", PDFObject::createInteger(4));
        set(annotation, "NM", PDFObject::createString(QUuid::createUuid().toByteArray()));
        const QJsonObject meta{{"version", 1}, {"group", id}, {"settings", settings(options)}};
        set(annotation, metadataKey,
            PDFObject::createString(QJsonDocument(meta).toJson(QJsonDocument::Compact)));
        builder.setObject(reference, dictObject(annotation));
        if (progress)
            progress(++completed, layout.size());
    }
    checkCancel(cancelled);
    return builder.build();
}
} // namespace
QVector<DecorationGroup> decorationGroups(const PDFDocument& document)
{
    QVector<DecorationGroup> result;
    for (const auto& item : owned(document))
    {
        auto found = std::find_if(result.begin(), result.end(),
                                  [&](const auto& value) { return value.id == item.id; });
        if (found == result.end())
            result << DecorationGroup{item.id, item.options, {item.page}};
        else
        {
            if (settings(found->options) != settings(item.options) ||
                found->pages.contains(item.page))
                fail("同じグループの設定が一致しません。ページ装飾を変更していません。");
            found->pages << item.page;
        }
    }
    return result;
}
PDFDocument putDecoration(const PDFDocument& document, const QVector<int>& pages,
                          const DecorationOptions& options, const QString& group,
                          const std::function<bool()>& cancelled,
                          const std::function<void(int, int)>& progress)
{
    return build(document, pages, options, group, -1, cancelled, progress);
}
PDFDocument previewDecoration(const PDFDocument& document, const QVector<int>& pages,
                              int previewPage, const DecorationOptions& options,
                              const QString& group, const std::function<bool()>& cancelled)
{
    if (previewPage < 0)
        fail("プレビューページを指定してください。");
    return build(document, pages, options, group, previewPage, cancelled, {});
}
PDFDocument removeDecoration(const PDFDocument& document, const QString& group,
                             const std::function<bool()>& cancelled)
{
    editableDocument(document);
    decorationGroups(document);
    PDFDocumentBuilder builder(&document);
    bool found = false;
    for (const auto& item : owned(document))
    {
        checkCancel(cancelled);
        if (item.id != group || group.isEmpty())
            continue;
        found = true;
        builder.removeAnnotation(document.getCatalog()->getPage(item.page)->getPageReference(),
                                 item.reference);
    }
    if (!found)
        fail("削除するページ装飾が見つかりません。");
    checkCancel(cancelled);
    return builder.build();
}
} // namespace tatsu
