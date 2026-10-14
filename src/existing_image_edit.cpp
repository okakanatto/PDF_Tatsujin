#include "existing_image_edit.h"
#include "existing_form_image_edit.h"
#include "image_embedding.h"
#include "pdf_objects.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include "pdfpagecontentprocessor.h"
#include "pdfparser.h"
#include "pdfrenderer.h"
#include "pdftextlayoutgenerator.h"
#include <algorithm>
#include <cmath>

namespace tatsu
{
namespace
{
using namespace detail;
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("画像の編集を中止しました。");
}
QByteArray contents(const PDFDocument& document, PDFObject value)
{
    value = document.getObject(value);
    QByteArray result;
    std::vector<PDFObject> entries;
    if (value.isArray())
        for (const auto& entry : *value.getArray())
            entries.push_back(entry);
    else if (!value.isNull())
        entries.push_back(value);
    for (const auto& entry : entries)
    {
        const auto stream = document.getObject(entry);
        if (!stream.isStream())
            fail("ページ内容の形式を確認できません。");
        result += document.getDecodedStream(stream.getStream()) + '\n';
        if (result.size() > 64 * 1024 * 1024)
            fail("画像を編集するページ内容が上限を超えています。");
    }
    return result;
}
struct Drawing
{
    qsizetype begin = 0, end = 0;
    QByteArray name;
};
QVector<Drawing> drawingSpans(const QByteArray& data, const std::function<bool()>& cancelled)
{
    PDFLexicalAnalyzer lexer(data.constBegin(), data.constEnd());
    QVector<Drawing> result;
    Drawing operand;
    bool nameOperand = false;
    while (!lexer.isAtEnd())
    {
        stop(cancelled);
        lexer.skipWhitespaceAndComments();
        const auto start = lexer.pos();
        const auto token = lexer.fetch();
        if (token.type == PDFLexicalAnalyzer::TokenType::Command)
        {
            const auto command = token.data.toByteArray();
            if (command == "BI" || command == "W" || command == "W*")
                fail("インライン画像やクリッピングがあるページの画像編集は未対応です。");
            if (command == "Do")
            {
                if (!nameOperand || result.size() >= 1000)
                    fail("画像の描画命令を確認できません。");
                operand.end = lexer.pos();
                result << operand;
            }
        }
        nameOperand = token.type == PDFLexicalAnalyzer::TokenType::Name;
        if (nameOperand)
            operand = {start, lexer.pos(), token.data.toByteArray()};
    }
    return result;
}
class ImageCollector : public PDFPageContentProcessor
{
public:
    using PDFPageContentProcessor::PDFPageContentProcessor;
    QVector<ExistingImage> images;
    int drawings = 0;
    QTransform physicalMatrix;
    std::function<bool()> cancelled;

protected:
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        return kind == ContentKind::Forms || PDFPageContentProcessor::isContentKindSuppressed(kind);
    }
    bool performOriginalImagePainting(const PDFImage&, const PDFStream*,
                                      PDFObjectReference) override
    {
        return true;
    }
    void performInterceptInstruction(Operator, ProcessOrder order,
                                     const QByteArray& command) override
    {
        if (order != ProcessOrder::BeforeOperation)
            return;
        stop(cancelled);
        if (command != "Do")
            return;
        const int occurrence = drawings++;
        if (getOperands().size() != 1 || !getXObjectDictionary())
            fail("画像のリソース参照が不正です。");
        const auto name = getOperands()[0].data.toByteArray();
        const auto reference = getXObjectDictionary()->get(name);
        const auto object = getDocument()->getObject(reference);
        if (!reference.isReference() || !object.isStream())
            fail("画像を描くオブジェクトが不正です。");
        const auto dictionary = object.getStream()->getDictionary();
        const auto subtype = getDocument()->getObject(dictionary->get("Subtype"));
        if (subtype.isName() && subtype.getString() == "Form")
        {
            return;
        }
        if (!subtype.isName() || subtype.getString() != "Image" || dictionary->hasKey("OC"))
            fail("この画像形式やレイヤーの編集は未対応です。");
        const auto width = getDocument()->getObject(dictionary->get("Width"));
        const auto height = getDocument()->getObject(dictionary->get("Height"));
        if (!width.isInt() || !height.isInt() || width.getInteger() <= 0 ||
            height.getInteger() <= 0 || width.getInteger() > 40000 || height.getInteger() > 40000 ||
            width.getInteger() * height.getInteger() > 160000000)
            fail("画像の画素寸法が編集の上限を超えています。");
        const auto matrix = getGraphicState()->getCurrentTransformationMatrix();
        const auto physical = (matrix * physicalMatrix).mapRect(QRectF(0, 0, 1, 1));
        if (!matrix.isInvertible() || !physical.isValid() || !std::isfinite(physical.left()) ||
            !std::isfinite(physical.top()) || !std::isfinite(physical.right()) ||
            !std::isfinite(physical.bottom()))
            fail("画像の座標変換を確認できません。");
        images << ExistingImage{
            occurrence, name,     reference.getReference(),
            matrix,     physical, QSize(int(width.getInteger()), int(height.getInteger()))};
    }
};
struct Inspection
{
    QByteArray bytes;
    QVector<Drawing> drawings;
    QVector<ExistingImage> images;
    QVector<QRectF> invisible;
};
class InvisibleTextProbe : public PDFTextLayoutGenerator
{
public:
    using PDFTextLayoutGenerator::PDFTextLayoutGenerator;
    QVector<QRectF> boxes;

protected:
    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        if (!isContentSuppressed() &&
            getGraphicState()->getTextRenderingMode() == TextRenderingMode::Invisible)
        {
            const auto box =
                getPagePointToDevicePointMatrix().map(info.matrix.map(info.outline)).boundingRect();
            if (box.isValid())
                boxes << box;
        }
    }
};
Inspection inspect(const PDFDocument& document, int page, const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (page < 0 || page >= int(document.getCatalog()->getPageCount()))
        fail("画像を編集するページを確認してください。");
    const auto root = document.getObject(document.getTrailerDictionary()->get("Root"));
    if (!root.isDictionary() || root.getDictionary()->hasKey("OCProperties") ||
        root.getDictionary()->hasKey("StructTreeRoot"))
        fail("レイヤーや構造タグがあるPDFの画像編集は未対応です。");
    const auto sourcePage = document.getCatalog()->getPage(page);
    Inspection result;
    result.bytes = contents(document, sourcePage->getContents());
    result.drawings = drawingSpans(result.bytes, cancelled);
    PDFFontCache fonts{128, 128};
    auto snapshot = document;
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    ImageCollector collector(sourcePage, &document, &fonts, cms.data(), nullptr, {}, {});
    collector.cancelled = cancelled;
    collector.physicalMatrix = pageMatrix(sourcePage);
    for (const auto& error : collector.processContents())
        if (error.type != RenderErrorType::Information)
            fail("画像を編集するページの解析に失敗しました: " + error.message);
    if (collector.drawings != result.drawings.size())
        fail("画像命令の対応を確認できません。");
    result.images = collector.images;
    InvisibleTextProbe text(PDFRenderer::getDefaultFeatures(), sourcePage, &document, &fonts,
                            cms.data(), nullptr, collector.physicalMatrix, {});
    for (const auto& error : text.processContents())
        if (error.type != RenderErrorType::Information)
            fail("既存文字層の位置を確認できません: " + error.message);
    result.invisible = text.boxes;
    return result;
}
QByteArray matrixBytes(const QTransform& matrix)
{
    QByteArray result;
    for (double value :
         {matrix.m11(), matrix.m12(), matrix.m21(), matrix.m22(), matrix.dx(), matrix.dy()})
    {
        if (!std::isfinite(value))
            fail("変更後の画像の座標が不正です。");
        result += QByteArray::number(value, 'f', 17) + ' ';
    }
    return result + "cm\n";
}
} // namespace
QVector<ExistingImage> existingImages(const PDFDocument& document, int page,
                                      const std::function<bool()>& cancelled, bool includeForms)
{
    if (includeForms)
        return formImageDrawings(document, page, cancelled);
    return inspect(document, page, cancelled).images;
}
PDFDocument editExistingImage(const PDFDocument& snapshot, int page, int occurrence,
                              ExistingImageChange change, QRectF physical,
                              const QImage& replacement, const std::function<bool()>& cancelled,
                              bool includeForms)
{
    if (includeForms)
        return editFormImageDrawing(snapshot, page, occurrence, change, physical, replacement,
                                    cancelled);
    const auto restriction = editingRestriction(snapshot);
    if (!restriction.isEmpty())
        fail(restriction);
    if (change != ExistingImageChange::Geometry && change != ExistingImageChange::Replace &&
        change != ExistingImageChange::Remove)
        fail("画像の編集操作を確認してください。");
    const auto inspected = inspect(snapshot, page, cancelled);
    const auto found =
        std::find_if(inspected.images.cbegin(), inspected.images.cend(),
                     [&](const auto& image) { return image.occurrence == occurrence; });
    if (found == inspected.images.cend())
        fail("編集対象の画像が見つかりません。Form内部の画像は未対応です。");
    if (std::any_of(inspected.invisible.cbegin(), inspected.invisible.cend(),
                    [&](const auto& box)
                    {
                        return box.intersects(found->physical) ||
                               (change != ExistingImageChange::Remove && box.intersects(physical));
                    }))
        fail("対象画像に対応する既存OCRの位置・内容と一致しなくなるため、この画像は編集できません"
             "。");
    const auto sourcePage = snapshot.getCatalog()->getPage(page);
    PDFDocumentBuilder builder(&snapshot);
    auto dictionary =
        *snapshot.getObjectByReference(sourcePage->getPageReference()).getDictionary();
    QByteArray invocation;
    const auto span = inspected.drawings.at(occurrence);
    if (change != ExistingImageChange::Remove)
    {
        const QRectF frame(QPointF(), pageSize(sourcePage));
        if (!physical.isValid() || !std::isfinite(physical.left()) ||
            !std::isfinite(physical.top()) || !std::isfinite(physical.right()) ||
            !std::isfinite(physical.bottom()) || !frame.contains(physical) ||
            physical.width() < 1 || physical.height() < 1)
            fail("画像の位置と大きさをページ範囲内で指定してください。");
        const auto transform = pageMatrix(sourcePage);
        const double sx = physical.width() / found->physical.width(),
                     sy = physical.height() / found->physical.height();
        const QTransform delta(sx, 0, 0, sy, physical.x() - sx * found->physical.x(),
                               physical.y() - sy * found->physical.y());
        const auto changedMatrix = found->matrix * transform * delta * transform.inverted();
        const auto relative = changedMatrix * found->matrix.inverted();
        QByteArray draw = inspected.bytes.mid(span.begin, span.end - span.begin);
        if (change == ExistingImageChange::Replace)
        {
            if (replacement.isNull() ||
                qint64(replacement.width()) * replacement.height() > 160000000)
                fail("差替えるPNG/JPEG画像を確認してください。");
            auto resources = *snapshot.getDictionaryFromObject(sourcePage->getResources());
            auto xobjects = *snapshot.getDictionaryFromObject(resources.get("XObject"));
            int index = 1;
            QByteArray name;
            do
            {
                name = "TatsujinImage" + QByteArray::number(index++);
            } while (xobjects.hasKey(name));
            xobjects.setEntry(PDFInplaceOrMemoryString(name),
                              PDFObject::createReference(embedImage(builder, replacement)));
            set(resources, "XObject", dictObject(xobjects));
            set(dictionary, "Resources", dictObject(resources));
            draw = '/' + name + " Do";
        }
        invocation = "q\n" + matrixBytes(relative) + draw + "\nQ\n";
    }
    const auto changed =
        inspected.bytes.left(span.begin) + invocation + inspected.bytes.mid(span.end);
    set(dictionary, "Contents",
        PDFObject::createReference(builder.addObject(streamObject({}, changed))));
    builder.setObject(sourcePage->getPageReference(), dictObject(dictionary));
    stop(cancelled);
    return builder.build();
}
} // namespace tatsu
