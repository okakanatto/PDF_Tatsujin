#include "form_content_invocations.h"
#include "pdf_objects.h"
#include "pdfparser.h"
#include <cmath>
#include <limits>

namespace tatsu
{
namespace
{
using namespace detail;
using Drawing = ContentSpan;
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
} // namespace
using namespace detail;
bool contentBoundsContain(const QVector<QPolygonF>& clips, const QPolygonF& shape)
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

FormContentInvocations::FormContentInvocations(const PDFDocument& source, int page,
                                               std::function<void()> cancelled)
    : document(source), checkCancelled(std::move(cancelled))
{
    if (page < 0 || page >= int(document.getCatalog()->getPageCount()))
        fail("描画のページを確認してください。");
    if (checkCancelled)
        checkCancelled();
    const auto sourcePage = document.getCatalog()->getPage(page);
    TrackedContentOwner owner;
    owner.reference = sourcePage->getPageReference();
    owner.attributes = dictionary(document, PDFObject::createReference(owner.reference));
    owner.resources = dictionary(document, sourcePage->getResources());
    owner.bytes = pageContents(document, sourcePage->getContents());
    owner.drawings = drawings(owner.bytes);
    decodedBytes = owner.bytes.size();
    physicalMatrix = pageMatrix(sourcePage);
    owners << owner;
}
XObjectDrawing FormContentInvocations::beginDrawing(const QByteArray& name,
                                                    const PDFDictionary* xobjects,
                                                    const QTransform& matrix)
{
    if (checkCancelled)
        checkCancelled();
    pushed << false;
    if (count >= 1000 || !xobjects)
        fail("グループの描画数や参照が編集の上限を超えています。");
    const int occurrence = count++, parent = active.last();
    if (owners[parent].cursor >= owners[parent].drawings.size())
        fail("グループの描画命令が一致しません。");
    const auto span = owners[parent].drawings[owners[parent].cursor++];
    const auto reference = xobjects->get(name);
    const auto object = document.getObject(reference);
    if (!reference.isReference() || !object.isStream())
        fail("グループの描画オブジェクトを確認できません。");
    const auto attributes = *object.getStream()->getDictionary();
    const auto subtype = document.getObject(attributes.get("Subtype"));
    if (!subtype.isName() || attributes.hasKey("OC"))
        fail("この描画形式やレイヤーの編集は未対応です。");
    const XObjectDrawing drawing{
        name, subtype.getString(), reference.getReference(), attributes, parent, occurrence, span};
    if (subtype.getString() == "Form")
    {
        if (attributes.hasKey("Group"))
            fail("透明グループの内容編集は未対応です。");
        if (active.size() > 8)
            fail("描画グループが8階層を超えています。");
        for (int index : active)
            if (owners[index].reference == reference.getReference())
                fail("循環するグループの内容は編集できません。");
        QTransform formMatrix;
        if (!attributes.get("Matrix").isNull())
        {
            const auto v = numbers(document, attributes.get("Matrix"), 6);
            formMatrix = {v[0], v[1], v[2], v[3], v[4], v[5]};
        }
        const auto v = numbers(document, attributes.get("BBox"), 4);
        const QRectF box(v[0], v[1], v[2] - v[0], v[3] - v[1]);
        if (!formMatrix.isInvertible() || !matrix.isInvertible() || !box.isValid())
            fail("グループの行列や表示範囲が不正です。");
        TrackedContentOwner owner;
        owner.reference = reference.getReference();
        owner.attributes = attributes;
        owner.resources = attributes.get("Resources").isNull()
                              ? owners[parent].resources
                              : dictionary(document, attributes.get("Resources"));
        owner.bytes = document.getDecodedStream(object.getStream());
        decodedBytes += owner.bytes.size();
        if (decodedBytes > 64 * 1024 * 1024)
            fail("グループの描画内容が編集の上限を超えています。");
        owner.drawings = drawings(owner.bytes);
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
        return drawing;
    }

    return drawing;
}
void FormContentInvocations::endDrawing()
{
    if (pushed.isEmpty())
        fail("グループの呼出し対応を確認できません。");
    if (pushed.takeLast())
    {
        const auto& owner = owners[active.takeLast()];
        if (owner.cursor != owner.drawings.size())
            fail("グループの描画対応を確認できません。");
    }
}
bool FormContentInvocations::finished() const
{
    return active.size() == 1 && pushed.isEmpty() && owners[0].cursor == owners[0].drawings.size();
}
} // namespace tatsu
