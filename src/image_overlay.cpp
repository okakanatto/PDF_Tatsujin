#include "document.h"
#include "image_embedding.h"
#include "pdf_objects.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "pdfimage.h"
#include <cmath>

namespace tatsu
{
using namespace detail;
QImage overlayImage(const PDFDocument& document, const Signature& item)
try
{
    if (!isImage(item.kind))
        fail("画像を選択してください。");
    auto annotation = document.getObjectByReference(item.ref);
    if (!annotation.isDictionary())
        fail("画像の注釈が見つかりません。");
    auto appearance = document.getObject(annotation.getDictionary()->get("AP"));
    if (!appearance.isDictionary())
        fail("画像の外観がありません。");
    auto normal = document.getObject(appearance.getDictionary()->get("N"));
    if (!normal.isStream())
        fail("画像の外観形式に対応していません。");
    auto resources = document.getObject(normal.getStream()->getDictionary()->get("Resources"));
    if (!resources.isDictionary())
        fail("画像のリソースがありません。");
    auto objects = document.getObject(resources.getDictionary()->get("XObject"));
    if (objects.isDictionary())
        for (size_t i = 0; i < objects.getDictionary()->getCount(); ++i)
        {
            auto object = document.getObject(objects.getDictionary()->getValue(i));
            if (!object.isStream())
                continue;
            auto dictionary = object.getStream()->getDictionary();
            auto subtype = document.getObject(dictionary->get("Subtype"));
            if (!subtype.isName() || subtype.getString() != "Image")
                continue;
            auto color = PDFAbstractColorSpace::createColorSpace(
                nullptr, &document, document.getObject(dictionary->get("ColorSpace")));
            PDFCMSManager cms(nullptr);
            PDFRenderErrorReporterDummy reporter;
            auto decoded = PDFImage::createImage(&document, object.getStream(), color, false,
                                                 RenderingIntent::RelativeColorimetric, &reporter);
            return decoded.getImage(cms.getCurrentCMS().data(), &reporter, nullptr);
        }
    fail("保存済み画像を読み出せませんでした。");
}
catch (const PDFException& error)
{
    fail("保存済み画像を読み出せません: " + error.getMessage());
}
Signature Document::putImage(int page, OverlayKind kind, const QImage& image, QPointF point,
                             double widthPoints, PDFObjectReference old)
{
    editable();
    if (!isImage(kind) || image.isNull() || !std::isfinite(widthPoints) || widthPoints < 6)
        fail("画像と幅を指定してください。");
    const double unit = pdf().getCatalog()->getPage(page)->getUserUnit();
    const QSizeF dimensions(widthPoints / unit,
                            widthPoints / unit * image.height() / image.width());
    const QRectF rectangle(point, dimensions);
    if (!pdf().getCatalog()->getPage(page)->getCropBox().contains(rectangle))
        fail("画像がページ範囲を超えます。位置か幅を変更してください。");
    PDFDocumentBuilder builder(&pdf());
    if (old.isValid())
        builder.removeAnnotation(pdf().getCatalog()->getPage(page)->getPageReference(), old);
    auto imageReference = embedImage(builder, image);
    PDFDictionary xobjects;
    set(xobjects, "Image", PDFObject::createReference(imageReference));
    PDFDictionary resources;
    set(resources, "XObject", dictObject(xobjects));
    const auto commands = QString("q %1 0 0 %2 0 0 cm /Image Do Q\n")
                              .arg(dimensions.width(), 0, 'g', 14)
                              .arg(dimensions.height(), 0, 'g', 14)
                              .toLatin1();
    PDFDictionary form;
    set(form, "Type", PDFObject::createName("XObject"));
    set(form, "Subtype", PDFObject::createName("Form"));
    set(form, "BBox", rectObject(QRectF(QPointF(), dimensions)));
    set(form, "Resources", dictObject(resources));
    const auto appearanceReference = builder.addObject(streamObject(form, commands));
    PDFDictionary ap;
    set(ap, "N", PDFObject::createReference(appearanceReference));
    const auto reference = builder.createAnnotationStamp(
        pdf().getCatalog()->getPage(page)->getPageReference(), rectangle, Stamp::Approved,
        "PDF達人", kind == OverlayKind::SignatureImage ? "見た目の画像署名" : "画像", {});
    auto dictionary = *builder.getObjectByReference(reference).getDictionary();
    set(dictionary, "Rect", rectObject(rectangle));
    set(dictionary, "AP", dictObject(ap));
    set(dictionary, "F", PDFObject::createInteger(4));
    const QJsonObject metadata{{"version", 1},
                               {"kind", int(kind)},
                               {"pixelWidth", image.width()},
                               {"pixelHeight", image.height()}};
    set(dictionary, "Tatsujin",
        PDFObject::createString(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
    set(dictionary, "NM", PDFObject::createString(QUuid::createUuid().toByteArray()));
    builder.setObject(reference, dictObject(dictionary));
    commit(builder.build());
    return {reference, rectangle, {}, 0, {}, kind, image.size()};
}
void Document::resizeImage(int page, const Signature& image, double widthPoints)
{
    editable();
    if (!isImage(image.kind) || !image.rect.isValid() || !std::isfinite(widthPoints) ||
        widthPoints < 6)
        fail("画像と幅を指定してください。");
    const auto p = pdf().getCatalog()->getPage(page);
    const double width = widthPoints / p->getUserUnit();
    const QRectF rectangle(image.rect.topLeft(),
                           QSizeF(width, width * image.rect.height() / image.rect.width()));
    if (!p->getCropBox().contains(rectangle))
        fail("画像がページ範囲を超えます。位置か幅を変更してください。");
    PDFDocumentBuilder builder(&pdf());
    auto dictionary = *builder.getObjectByReference(image.ref).getDictionary();
    set(dictionary, "Rect", rectObject(rectangle));
    builder.setObject(image.ref, dictObject(dictionary));
    commit(builder.build());
}
} // namespace tatsu
