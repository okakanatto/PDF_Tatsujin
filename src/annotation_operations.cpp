#include "annotation_operations.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include <cmath>

namespace tatsu
{
using namespace detail;
void removeOwnedAnnotation(PDFDocumentBuilder& builder, PDFObjectReference page,
                           PDFObjectReference reference)
{
    const auto object = builder.getObjectByReference(reference);
    if (object.isDictionary())
    {
        const auto popup = object.getDictionary()->get("Popup");
        const auto popupObject = builder.getStorage()->getObject(popup);
        if (popup.isReference() && popupObject.isDictionary() &&
            popupObject.getDictionary()->get("Parent") == PDFObject::createReference(reference))
            builder.removeAnnotation(page, popup.getReference());
    }
    builder.removeAnnotation(page, reference);
}
void updateOwnedAnnotationAppearance(PDFDocumentBuilder& builder, PDFObjectReference reference)
{
    auto object = builder.getObjectByReference(reference);
    auto dictionary = *object.getDictionary();
    const auto metadata = QJsonDocument::fromJson(dictionary.get("Tatsujin").getString()).object();
    if (metadata["kind"].toInt() != int(OverlayKind::Rectangle))
    {
        builder.updateAnnotationAppearanceStreams(reference);
        return;
    }
    PDFDocumentDataLoaderDecorator loader(builder.getStorage());
    const auto rectangle = loader.readRectangle(dictionary.get("Rect"), {});
    const auto page = builder.getDictionaryFromObject(dictionary.get("P"));
    const auto unit = page ? loader.readNumberFromDictionary(page, "UserUnit", 1) : 1;
    const double stroke = metadata["size"].toDouble() / unit;
    const QColor color(metadata["color"].toString());
    if (!rectangle.isValid() || stroke >= qMin(rectangle.width(), rectangle.height()))
        fail("矩形の幅・高さを線幅より大きくしてください。");
    auto number = [](double value) { return QByteArray::number(value, 'g', 12); };
    QByteArray commands = "q " + number(color.redF()) + " " + number(color.greenF()) + " " +
                          number(color.blueF()) + " RG " + number(stroke) + " w " +
                          number(stroke / 2) + " " + number(stroke / 2) + " " +
                          number(rectangle.width() - stroke) + " " +
                          number(rectangle.height() - stroke) + " re S Q\n";
    PDFDictionary appearance;
    set(appearance, "Type", PDFObject::createName("XObject"));
    set(appearance, "Subtype", PDFObject::createName("Form"));
    set(appearance, "BBox", rectObject(QRectF(QPointF(), rectangle.size())));
    set(appearance, "Resources", dictObject({}));
    const auto stream = builder.addObject(streamObject(appearance, commands));
    PDFDictionary normal;
    set(normal, "N", PDFObject::createReference(stream));
    set(dictionary, "AP", dictObject(normal));
    set(dictionary, "IC", PDFObject());
    builder.setObject(reference, dictObject(dictionary));
}
void completeAnnotationStreams(PDFDocumentBuilder& builder)
{
    const auto& objects = builder.getStorage()->getObjects();
    for (size_t i = 0; i < objects.size(); ++i)
    {
        const auto object = objects[i].object;
        if (!object.isStream() || object.getStream()->getDictionary()->hasKey("Length"))
            continue;
        builder.setObject(
            {PDFInteger(i), objects[i].generation},
            streamObject(*object.getStream()->getDictionary(), *object.getStream()->getContent()));
    }
}
namespace
{
Signature create(PDFDocumentBuilder& builder, const PDFDocument& document, int page,
                 OverlayKind kind, QRectF rectangle, const QString& contents, QColor color,
                 double width, QPolygonF geometry, PDFObjectReference old)
{
    if (!isAnnotation(kind) || !color.isValid() || !std::isfinite(width) || width <= 0 ||
        width > 24)
        fail("注釈の種類・色・線幅を指定してください。");
    if (page < 0 || page >= int(document.getCatalog()->getPageCount()))
        fail("注釈の対象ページが不正です。");
    const auto p = document.getCatalog()->getPage(page);
    if (!rectangle.isValid() || !p->getCropBox().contains(rectangle))
        fail("注釈をページ内に配置してください。");
    if (old.isValid())
        removeOwnedAnnotation(builder, p->getPageReference(), old);
    PDFObjectReference reference;
    switch (kind)
    {
    case OverlayKind::Comment:
        if (contents.trimmed().isEmpty())
            fail("コメントを入力してください。");
        reference = builder.createAnnotationText(p->getPageReference(), rectangle,
                                                 TextAnnotationIcon::Comment, "PDF達人", "コメント",
                                                 contents, false);
        break;
    case OverlayKind::Rectangle:
        reference = builder.createAnnotationSquare(p->getPageReference(), rectangle,
                                                   width / p->getUserUnit(), {}, color, "PDF達人",
                                                   "矩形", contents);
        break;
    case OverlayKind::Line:
    case OverlayKind::Arrow:
        if (geometry.size() != 2 || QLineF(geometry[0], geometry[1]).length() < .5)
            fail("線の始点と終点を指定してください。");
        reference = builder.createAnnotationLine(
            p->getPageReference(), rectangle, geometry[0], geometry[1], width / p->getUserUnit(),
            color, color, "PDF達人", kind == OverlayKind::Arrow ? "矢印" : "直線", contents,
            AnnotationLineEnding::None,
            kind == OverlayKind::Arrow ? AnnotationLineEnding::ClosedArrow
                                       : AnnotationLineEnding::None);
        break;
    case OverlayKind::Highlight:
        if (geometry.isEmpty() || geometry.size() % 4 != 0)
            fail("選択した文字範囲がありません。スキャンには先にOCRを実行してください。");
        reference = builder.createAnnotationHighlight(p->getPageReference(), geometry, color);
        break;
    default:
        fail("注釈の種類が不正です。");
    }
    auto dictionary = *builder.getObjectByReference(reference).getDictionary();
    PDFObjectFactory colorObject;
    colorObject << color;
    set(dictionary, "C", colorObject.takeObject());
    set(dictionary, "F", PDFObject::createInteger(4));
    set(dictionary, "NM", PDFObject::createString(QUuid::createUuid().toByteArray()));
    PDFObjectFactory text;
    text << contents;
    set(dictionary, "Contents", text.takeObject());
    if (kind == OverlayKind::Rectangle)
        set(dictionary, "Rect", rectObject(rectangle));
    const QJsonObject metadata{{"version", 1},
                               {"kind", int(kind)},
                               {"text", contents},
                               {"size", width},
                               {"color", color.name()}};
    set(dictionary, "Tatsujin",
        PDFObject::createString(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
    builder.setObject(reference, dictObject(dictionary));
    updateOwnedAnnotationAppearance(builder, reference);
    PDFDocumentDataLoaderDecorator loader(builder.getStorage());
    const auto actual = builder.getObjectByReference(reference);
    const auto rect = loader.readRectangle(actual.getDictionary()->get("Rect"), rectangle);
    return {reference, rect, contents, width, color, kind, {}, geometry};
}
} // namespace
Signature putAnnotation(Document& document, int page, OverlayKind kind, QRectF rectangle,
                        const QString& contents, QColor color, double width, QPolygonF geometry,
                        PDFObjectReference old)
{
    document.editable();
    PDFDocumentBuilder builder(&document.pdf());
    auto annotation = create(builder, document.pdf(), page, kind, rectangle, contents, color, width,
                             geometry, old);
    completeAnnotationStreams(builder);
    document.commit(builder.build());
    return annotation;
}
QVector<PDFObjectReference>
putHighlights(Document& document, const QMap<int, QVector<QRectF>>& selections, QColor color)
{
    document.editable();
    if (selections.isEmpty())
        fail("本文の文字を選択してください。スキャンには先にOCRを実行してください。");
    PDFDocumentBuilder builder(&document.pdf());
    QVector<PDFObjectReference> references;
    for (auto it = selections.cbegin(); it != selections.cend(); ++it)
    {
        QRectF bounds;
        QPolygonF points;
        for (const auto& rectangle : it.value())
        {
            if (!rectangle.isValid())
                continue;
            bounds = bounds.united(rectangle);
            points << rectangle.bottomLeft() << rectangle.bottomRight() << rectangle.topLeft()
                   << rectangle.topRight();
        }
        if (!points.isEmpty())
            references.append(create(builder, document.pdf(), it.key(), OverlayKind::Highlight,
                                     bounds, {}, color, 1, points, {})
                                  .ref);
    }
    if (references.isEmpty())
        fail("選択した文字範囲がありません。");
    completeAnnotationStreams(builder);
    document.commit(builder.build());
    return references;
}
} // namespace tatsu
