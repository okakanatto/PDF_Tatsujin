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
struct Drawing
{
    qsizetype begin, end;
};
QVector<Drawing> drawings(const QByteArray& bytes)
{
    PDFLexicalAnalyzer lexer(bytes.constBegin(), bytes.constEnd());
    QVector<Drawing> result;
    bool named = false;
    qsizetype begin = 0;
    while (!lexer.isAtEnd())
    {
        lexer.skipWhitespaceAndComments();
        const auto position = lexer.pos();
        const auto token = lexer.fetch();
        if (token.type == PDFLexicalAnalyzer::TokenType::Command)
        {
            const auto command = token.data.toByteArray();
            if (command == "BI" || command == "W" || command == "W*")
                fail("インライン画像や明示クリップを含むグループの画像編集は未対応です。");
            if (command == "Do")
            {
                if (!named || result.size() >= 1000)
                    fail("グループの描画命令を確認できません。");
                result << Drawing{begin, lexer.pos()};
            }
        }
        named = token.type == PDFLexicalAnalyzer::TokenType::Name;
        if (named)
            begin = position;
    }
    return result;
}
double scalar(const PDFDocument& document, PDFObject value)
{
    value = document.getObject(value);
    const double result = value.isInt()    ? double(value.getInteger())
                          : value.isReal() ? value.getReal()
                                           : std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(result))
        fail("グループの座標を確認できません。");
    return result;
}
QVector<double> numbers(const PDFDocument& document, PDFObject value, int count)
{
    value = document.getObject(value);
    if (!value.isArray() || value.getArray()->getCount() != size_t(count))
        fail("グループの座標と表示範囲を確認できません。");
    QVector<double> result;
    for (const auto& entry : *value.getArray())
        result << scalar(document, entry);
    return result;
}
PDFDictionary dictionary(const PDFDocument& document, PDFObject value)
{
    value = document.getObject(value);
    if (!value.isDictionary())
        fail("グループの描画資源を確認できません。");
    return *value.getDictionary();
}
QByteArray pageContents(const PDFDocument& document, PDFObject value)
{
    value = document.getObject(value);
    std::vector<PDFObject> entries;
    if (value.isArray())
        for (const auto& entry : *value.getArray())
            entries.push_back(entry);
    else if (!value.isNull())
        entries.push_back(value);
    QByteArray bytes;
    for (const auto& entry : entries)
    {
        const auto stream = document.getObject(entry);
        if (!stream.isStream())
            fail("ページの描画を確認できません。");
        bytes += document.getDecodedStream(stream.getStream()) + '\n';
        if (bytes.size() > 64 * 1024 * 1024)
            fail("描画内容が編集の上限を超えています。");
    }
    return bytes;
}
struct Owner
{
    PDFObjectReference reference;
    PDFDictionary attributes, resources;
    QByteArray bytes;
    QVector<Drawing> spans;
    int cursor = 0, parent = -1;
    Drawing parentSpan{};
    QVector<QPolygonF> clips;
};
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
bool contains(const QVector<QPolygonF>& clips, const QPolygonF& shape)
{
    for (const auto& clip : clips)
        for (const auto& corner : shape)
        {
            if (!clip.containsPoint(corner, Qt::OddEvenFill))
            {
                bool boundary = false;
                for (int i = 0; i < clip.size(); ++i)
                {
                    const auto a = clip[i], delta = clip[(i + 1) % clip.size()] - a,
                               point = corner - a;
                    const double length = delta.x() * delta.x() + delta.y() * delta.y();
                    if (!length)
                        continue;
                    const double position =
                        (point.x() * delta.x() + point.y() * delta.y()) / length;
                    const auto nearest = a + qBound(0.0, position, 1.0) * delta;
                    // Inclusive edges with only floating-point arithmetic tolerance.
                    if (QLineF(nearest, corner).length() <= 1e-8)
                    {
                        boundary = true;
                        break;
                    }
                }
                if (!boundary)
                    return false;
            }
        }
    return true;
}
class Walker : public PDFPageContentProcessor
{
public:
    using PDFPageContentProcessor::PDFPageContentProcessor;
    QVector<Owner> owners;
    QVector<Leaf> leaves;
    QVector<int> active{0};
    QVector<bool> pushed;
    QTransform physicalMatrix;
    std::function<bool()> cancelled;
    int count = 0;
    qsizetype decodedBytes = 0;

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
            if (pushed.takeLast())
            {
                const auto& finished = owners[active.takeLast()];
                if (finished.cursor != finished.spans.size())
                    fail("グループの描画対応を確認できません。");
            }
            return;
        }
        pushed << false;
        if (count >= 1000 || getOperands().size() != 1 || !getXObjectDictionary())
            fail("グループの描画数や参照が編集の上限を超えています。");
        const int occurrence = count++, parent = active.last();
        if (owners[parent].cursor >= owners[parent].spans.size())
            fail("グループの描画命令が一致しません。");
        const auto span = owners[parent].spans[owners[parent].cursor++];
        const auto name = getOperands()[0].data.toByteArray();
        const auto reference = getXObjectDictionary()->get(name);
        const auto object = getDocument()->getObject(reference);
        if (!reference.isReference() || !object.isStream())
            fail("グループの描画オブジェクトを確認できません。");
        const auto attributes = *object.getStream()->getDictionary();
        const auto subtype = getDocument()->getObject(attributes.get("Subtype"));
        if (!subtype.isName() || attributes.hasKey("OC"))
            fail("この描画形式やレイヤーの編集は未対応です。");
        const auto matrix = getGraphicState()->getCurrentTransformationMatrix();
        if (subtype.getString() == "Form")
        {
            if (attributes.hasKey("Group"))
                fail("透明グループ内の画像編集は未対応です。");
            if (active.size() > 8)
                fail("画像を含むグループが8階層を超えています。");
            for (int index : active)
                if (owners[index].reference == reference.getReference())
                    fail("循環するグループ内の画像は編集できません。");
            QTransform formMatrix;
            if (!attributes.get("Matrix").isNull())
            {
                const auto v = numbers(*getDocument(), attributes.get("Matrix"), 6);
                formMatrix = {v[0], v[1], v[2], v[3], v[4], v[5]};
            }
            const auto v = numbers(*getDocument(), attributes.get("BBox"), 4);
            const QRectF box(v[0], v[1], v[2] - v[0], v[3] - v[1]);
            if (!formMatrix.isInvertible() || !matrix.isInvertible() || !box.isValid())
                fail("グループの行列や表示範囲が不正です。");
            Owner owner;
            owner.reference = reference.getReference();
            owner.attributes = attributes;
            owner.resources = attributes.get("Resources").isNull()
                                  ? owners[parent].resources
                                  : dictionary(*getDocument(), attributes.get("Resources"));
            owner.bytes = getDocument()->getDecodedStream(object.getStream());
            decodedBytes += owner.bytes.size();
            if (decodedBytes > 64 * 1024 * 1024)
                fail("グループの描画内容が編集の上限を超えています。");
            owner.spans = drawings(owner.bytes);
            owner.parent = parent;
            owner.parentSpan = span;
            owner.clips = owners[parent].clips;
            const auto clip = (formMatrix * matrix * physicalMatrix).map(QPolygonF(box));
            for (const auto& point : clip)
                if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                    fail("グループの表示範囲が不正です。");
            owner.clips << clip;
            active << owners.size();
            owners << owner;
            pushed.last() = true;
            return;
        }
        if (subtype.getString() != "Image")
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
        if (!contains(owners[parent].clips,
                      (matrix * physicalMatrix).map(QPolygonF(QRectF(0, 0, 1, 1)))))
            fail("グループの表示範囲で切り取られた画像の編集は未対応です。");
        Leaf leaf{{occurrence, name, reference.getReference(), matrix, physical,
                   QSize(int(width.getInteger()), int(height.getInteger()))},
                  parent,
                  span,
                  matrix,
                  owners[parent].bytes.mid(span.begin, span.end - span.begin)};
        leaf.image.depth = active.size() - 1;
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
    Owner owner;
    owner.reference = sourcePage->getPageReference();
    owner.attributes = dictionary(document, PDFObject::createReference(owner.reference));
    owner.resources = dictionary(document, sourcePage->getResources());
    owner.bytes = pageContents(document, sourcePage->getContents());
    owner.spans = drawings(owner.bytes);
    PDFFontCache fonts{128, 128};
    auto snapshot = document;
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    Walker walker(sourcePage, &document, &fonts, cms.data(), nullptr, {}, {});
    walker.owners << owner;
    walker.decodedBytes = owner.bytes.size();
    walker.physicalMatrix = pageMatrix(sourcePage);
    walker.cancelled = cancelled;
    for (const auto& error : walker.processContents())
        if (error.type != RenderErrorType::Information)
            fail("グループ内の画像を解析できません: " + error.message);
    if (walker.active.size() != 1 || walker.owners[0].cursor != owner.spans.size() ||
        !walker.pushed.isEmpty())
        fail("グループ内の画像の対応が一致しません。");
    Invisible text(PDFRenderer::getDefaultFeatures(), sourcePage, &document, &fonts, cms.data(),
                   nullptr, walker.physicalMatrix, {});
    for (const auto& error : text.processContents())
        if (error.type != RenderErrorType::Information)
            fail("既存OCRの座標を確認できません: " + error.message);
    stop(cancelled);
    return {walker.owners, walker.leaves, text.boxes};
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
        if (!contains(tree.owners[ownerIndex].clips,
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
    while (true)
    {
        stop(cancelled);
        auto& owner = tree.owners[ownerIndex];
        const auto bytes = owner.bytes.left(span.begin) + invocation + owner.bytes.mid(span.end);
        auto attributes = owner.attributes;
        set(attributes, "Resources", dictObject(owner.resources));
        if (ownerIndex == 0)
        {
            set(attributes, "Contents",
                PDFObject::createReference(builder.addObject(streamObject({}, bytes))));
            builder.setObject(owner.reference, dictObject(attributes));
            break;
        }
        attributes.removeEntry("Filter");
        attributes.removeEntry("DecodeParms");
        const auto reference = builder.addObject(streamObject(attributes, bytes));
        span = owner.parentSpan;
        ownerIndex = owner.parent;
        invocation =
            addResource(tree.owners[ownerIndex].resources, reference, snapshot, "TatsujinForm");
    }
    stop(cancelled);
    return builder.build();
}
} // namespace tatsu
