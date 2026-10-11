#include "existing_form_image_edit.h"
#include "content_invocation_edit.h"
#include "form_content_invocations.h"
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
#include <limits>

namespace tatsu
{
namespace
{
using namespace detail;
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("グループ内の画像編集を中止しました。");
}
using Drawing = ContentSpan;
PDFDictionary dictionary(const PDFDocument& document, PDFObject value)
{
    value = document.getObject(value);
    if (!value.isDictionary())
        fail("グループの描画資源を確認できません。");
    return *value.getDictionary();
}
using Owner = TrackedContentOwner;
struct Leaf
{
    ExistingImage image;
    int owner;
    Drawing span;
    QTransform basis;
    QByteArray draw;
};
QByteArray matrixCommand(const QTransform& matrix);
QJsonArray matrixValues(const QTransform& matrix)
{
    return {matrix.m11(), matrix.m12(), matrix.m21(), matrix.m22(), matrix.dx(), matrix.dy()};
}
bool closeMatrix(const QTransform& first, const QTransform& second)
{
    const auto a = matrixValues(first), b = matrixValues(second);
    for (int i = 0; i < 6; ++i)
        if (qAbs(a[i].toDouble() - b[i].toDouble()) > 1e-8)
            return false;
    return true;
}
constexpr auto placementStart = "%TatsujinImagePlacement ";
constexpr auto placementEnd = "%TatsujinImagePlacementEnd\n";
bool placement(const QByteArray& bytes, Drawing span, const QTransform& observed, Drawing& complete,
               QTransform& basis, QByteArray& draw)
{
    const auto begin = bytes.lastIndexOf(placementStart, span.begin);
    if (begin < 0 || (begin && bytes[begin - 1] != '\n'))
        return false;
    const auto lineEnd = bytes.indexOf('\n', begin), end = bytes.indexOf(placementEnd, span.end);
    if (lineEnd < 0 || lineEnd >= span.begin || end < 0 || end - lineEnd > 65536)
        return false;
    const auto json =
        QJsonDocument::fromJson(
            QByteArray::fromBase64(bytes.mid(begin + QByteArray(placementStart).size(),
                                             lineEnd - begin - QByteArray(placementStart).size())))
            .object();
    auto matrix = [&](const char* key, QTransform& result)
    {
        const auto v = json[key].toArray();
        if (v.size() != 6)
            return false;
        for (const auto& n : v)
            if (!n.isDouble() || !std::isfinite(n.toDouble()))
                return false;
        result = {v[0].toDouble(), v[1].toDouble(), v[2].toDouble(),
                  v[3].toDouble(), v[4].toDouble(), v[5].toDouble()};
        return result.isInvertible();
    };
    QTransform relative;
    if (!matrix("basis", basis) || !matrix("relative", relative) ||
        !closeMatrix(relative * basis, observed))
        return false;
    draw = QByteArray::fromBase64(json["draw"].toString().toLatin1());
    PDFLexicalAnalyzer lexer(draw.constBegin(), draw.constEnd());
    lexer.skipWhitespaceAndComments();
    const auto name = lexer.fetch();
    lexer.skipWhitespaceAndComments();
    const auto op = lexer.fetch();
    lexer.skipWhitespaceAndComments();
    if (name.type != PDFLexicalAnalyzer::TokenType::Name ||
        op.type != PDFLexicalAnalyzer::TokenType::Command || op.data.toByteArray() != "Do" ||
        !lexer.isAtEnd())
        return false;
    const auto body = "q\n" + matrixCommand(relative) + draw + "\nQ\n" + placementEnd;
    if (bytes.mid(lineEnd + 1, end + QByteArray(placementEnd).size() - lineEnd - 1) != body)
        return false;
    complete = {begin, end + QByteArray(placementEnd).size()};
    return true;
}
QByteArray markedDraw(const QTransform& basis, const QTransform& relative, const QByteArray& draw)
{
    const QJsonObject json{{"basis", matrixValues(basis)},
                           {"relative", matrixValues(relative)},
                           {"draw", QString::fromLatin1(draw.toBase64())}};
    return '\n' + QByteArray(placementStart) +
           QJsonDocument(json).toJson(QJsonDocument::Compact).toBase64() + "\nq\n" +
           matrixCommand(relative) + draw + "\nQ\n" + placementEnd;
}
class Walker : public PDFPageContentProcessor
{
public:
    using PDFPageContentProcessor::PDFPageContentProcessor;
    FormContentInvocations* invocations = nullptr;
    QVector<Leaf> leaves;
    QTransform physicalMatrix;
    std::function<bool()> cancelled;

protected:
    bool performOriginalImagePainting(const PDFImage&, const PDFStream*,
                                      PDFObjectReference) override
    {
        return true;
    }
    void performInterceptInstruction(Operator, ProcessOrder order,
                                     const QByteArray& command) override
    {
        if (command != "Do")
            return;
        stop(cancelled);
        if (order == ProcessOrder::AfterOperation)
        {
            invocations->endDrawing();
            return;
        }
        if (getOperands().size() != 1)
            fail("グループの描画参照を確認できません。");
        const auto matrix = getGraphicState()->getCurrentTransformationMatrix();
        const auto drawing = invocations->beginDrawing(getOperands()[0].data.toByteArray(),
                                                       getXObjectDictionary(), matrix);
        if (drawing.subtype == "Form")
            return;
        auto& owners = invocations->owners;
        const auto& attributes = drawing.attributes;
        const auto& name = drawing.name;
        const auto reference = PDFObject::createReference(drawing.reference);
        const int parent = drawing.owner, occurrence = drawing.occurrence;
        const auto span = drawing.span;
        if (drawing.subtype != "Image")
            fail("このグループの画像形式は未対応です。");
        const auto width = getDocument()->getObject(attributes.get("Width"));
        const auto height = getDocument()->getObject(attributes.get("Height"));
        if (!width.isInt() || !height.isInt() || width.getInteger() <= 0 ||
            height.getInteger() <= 0 || width.getInteger() > 40000 || height.getInteger() > 40000 ||
            width.getInteger() * height.getInteger() > 160000000)
            fail("画像の画素寸法が編集の上限を超えています。");
        const auto physical = (matrix * physicalMatrix).mapRect(QRectF(0, 0, 1, 1));
        if (!matrix.isInvertible() || !physical.isValid() || !std::isfinite(physical.left()) ||
            !std::isfinite(physical.top()) || !std::isfinite(physical.right()) ||
            !std::isfinite(physical.bottom()))
            fail("グループの画像座標を確認できません。");
        if (!contentBoundsContain(owners[parent].clips,
                                  (matrix * physicalMatrix).map(QPolygonF(QRectF(0, 0, 1, 1)))))
            fail("グループの表示範囲で切り取られた画像の編集は未対応です。");
        Leaf leaf{{occurrence, name, reference.getReference(), matrix, physical,
                   QSize(int(width.getInteger()), int(height.getInteger()))},
                  parent,
                  span,
                  matrix,
                  owners[parent].bytes.mid(span.begin, span.end - span.begin)};
        leaf.image.depth = invocations->depth();
        Drawing complete;
        QTransform basis;
        QByteArray draw;
        if (placement(owners[parent].bytes, span, matrix, complete, basis, draw))
        {
            leaf.span = complete;
            leaf.basis = basis;
            leaf.draw = draw;
        }
        leaves << leaf;
    }
};
class Invisible : public PDFTextLayoutGenerator
{
public:
    using PDFTextLayoutGenerator::PDFTextLayoutGenerator;
    QVector<QRectF> boxes;

protected:
    void performOutputCharacter(const PDFTextCharacterInfo& character) override
    {
        if (!isContentSuppressed() &&
            getGraphicState()->getTextRenderingMode() == TextRenderingMode::Invisible)
        {
            const auto box = getPagePointToDevicePointMatrix()
                                 .map(character.matrix.map(character.outline))
                                 .boundingRect();
            if (box.isValid())
                boxes << box;
        }
    }
};
struct Tree
{
    QVector<Owner> owners;
    QVector<Leaf> leaves;
    QVector<QRectF> invisible;
};
Tree inspect(const PDFDocument& document, int page, const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (page < 0 || page >= int(document.getCatalog()->getPageCount()))
        fail("画像のページを確認してください。");
    const auto root = dictionary(document, document.getTrailerDictionary()->get("Root"));
    if (root.hasKey("OCProperties") || root.hasKey("StructTreeRoot"))
        fail("レイヤーや構造タグのあるPDFの画像編集は未対応です。");
    const auto sourcePage = document.getCatalog()->getPage(page);
    FormContentInvocations invocations(document, page, [&] { stop(cancelled); });
    PDFFontCache fonts{128, 128};
    auto snapshot = document;
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    Walker walker(sourcePage, &document, &fonts, cms.data(), nullptr, {}, {});
    walker.invocations = &invocations;
    walker.physicalMatrix = invocations.physicalMatrix;
    walker.cancelled = cancelled;
    for (const auto& error : walker.processContents())
        if (error.type != RenderErrorType::Information)
            fail("グループ内の画像を解析できません: " + error.message);
    if (!invocations.finished())
        fail("グループ内の画像の対応が一致しません。");
    Invisible text(PDFRenderer::getDefaultFeatures(), sourcePage, &document, &fonts, cms.data(),
                   nullptr, walker.physicalMatrix, {});
    for (const auto& error : text.processContents())
        if (error.type != RenderErrorType::Information)
            fail("既存OCRの座標を確認できません: " + error.message);
    stop(cancelled);
    return {invocations.owners, walker.leaves, text.boxes};
}
QByteArray matrixCommand(const QTransform& matrix)
{
    QByteArray result;
    for (double value :
         {matrix.m11(), matrix.m12(), matrix.m21(), matrix.m22(), matrix.dx(), matrix.dy()})
    {
        if (!std::isfinite(value))
            fail("画像の変換が不正です。");
        result += QByteArray::number(value, 'f', 17) + ' ';
    }
    return result + "cm\n";
}
QByteArray addResource(PDFDictionary& resources, PDFObjectReference reference,
                       const PDFDocument& document, const QByteArray& prefix)
{
    auto xobjects = dictionary(document, resources.get("XObject"));
    int number = 1;
    QByteArray name;
    do
    {
        name = prefix + QByteArray::number(number++);
    } while (xobjects.hasKey(name));
    xobjects.setEntry(PDFInplaceOrMemoryString(name), PDFObject::createReference(reference));
    set(resources, "XObject", dictObject(xobjects));
    return '/' + name + " Do";
}
} // namespace
QVector<ExistingImage> formImageDrawings(const PDFDocument& document, int page,
                                         const std::function<bool()>& cancelled)
{
    QVector<ExistingImage> result;
    const auto tree = inspect(document, page, cancelled);
    for (const auto& leaf : tree.leaves)
        result << leaf.image;
    return result;
}
PDFDocument editFormImageDrawing(const PDFDocument& snapshot, int page, int occurrence,
                                 ExistingImageChange change, QRectF physical,
                                 const QImage& replacement, const std::function<bool()>& cancelled)
{
    const auto restriction = editingRestriction(snapshot);
    if (!restriction.isEmpty())
        fail(restriction);
    if (change != ExistingImageChange::Geometry && change != ExistingImageChange::Replace &&
        change != ExistingImageChange::Remove)
        fail("画像の変更内容を確認してください。");
    auto tree = inspect(snapshot, page, cancelled);
    const auto found =
        std::find_if(tree.leaves.begin(), tree.leaves.end(), [occurrence](const auto& leaf)
                     { return leaf.image.occurrence == occurrence; });
    if (found == tree.leaves.end())
        fail("グループ内の対象画像が見つかりません。");
    const auto selected = *found;
    for (const auto& box : tree.invisible)
        if (box.intersects(selected.image.physical) ||
            (change != ExistingImageChange::Remove && box.intersects(physical)))
            fail("対象画像に対応する既存OCRと一致しなくなるため、この画像は編集できません。");
    PDFDocumentBuilder builder(&snapshot);
    QByteArray invocation;
    int ownerIndex = selected.owner;
    auto span = selected.span;
    if (change != ExistingImageChange::Remove)
    {
        const QRectF frame(QPointF(), pageSize(snapshot.getCatalog()->getPage(page)));
        if (!physical.isValid() || !std::isfinite(physical.left()) ||
            !std::isfinite(physical.top()) || !std::isfinite(physical.right()) ||
            !std::isfinite(physical.bottom()) || !frame.contains(physical) ||
            physical.width() < 1 || physical.height() < 1)
            fail("画像の位置と大きさをページ範囲内で指定してください。");
        const auto transform = pageMatrix(snapshot.getCatalog()->getPage(page));
        const auto source = (selected.basis * transform).mapRect(QRectF(0, 0, 1, 1));
        const double sx = physical.width() / source.width(),
                     sy = physical.height() / source.height();
        const QTransform delta(sx, 0, 0, sy, physical.x() - sx * source.x(),
                               physical.y() - sy * source.y());
        if (!contentBoundsContain(
                tree.owners[ownerIndex].clips,
                (selected.basis * transform * delta).map(QPolygonF(QRectF(0, 0, 1, 1)))))
            fail("画像がグループの表示範囲からはみ出します。");
        const auto changed = selected.basis * transform * delta * transform.inverted();
        auto draw = selected.draw;
        if (change == ExistingImageChange::Replace)
        {
            if (replacement.isNull() ||
                qint64(replacement.width()) * replacement.height() > 160000000)
                fail("差替える画像を確認してください。");
            draw = addResource(tree.owners[ownerIndex].resources, embedImage(builder, replacement),
                               snapshot, "TatsujinImage");
        }
        const auto relative = changed * selected.basis.inverted();
        invocation = physical == source ? draw : markedDraw(selected.basis, relative, draw);
    }
    QVector<ContentInvocationOwner> owners;
    for (const auto& owner : tree.owners)
        owners << owner;
    spliceContentInvocation(builder, snapshot, std::move(owners), ownerIndex, span, invocation,
                            [&] { stop(cancelled); });
    stop(cancelled);
    return builder.build();
}
} // namespace tatsu
