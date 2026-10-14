#include "redaction_content.h"
#include "image_embedding.h"
#include "pdf_objects.h"
#include "pdfcms.h"
#include "pdfdocumentwriter.h"
#include "pdffont.h"
#include "pdfpagecontentprocessor.h"
#include "pdfparser.h"
#include "redaction_cmap.h"
#include "redaction_font.h"
#include "redaction_resource_usage.h"
#include "redaction_standard_font.h"
#include "standard_font_metrics.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>

namespace tatsu
{
namespace
{
using namespace pdf;
QByteArray numeric(double value)
{
    if (!std::isfinite(value))
        fail("墨消し対象の座標が不正です。");
    return QByteArray::number(value, 'f', 10);
}
QByteArray tokenBytes(const PDFLexicalAnalyzer::Token& token)
{
    using Type = PDFLexicalAnalyzer::TokenType;
    switch (token.type)
    {
    case Type::Boolean:
        return token.data.toBool() ? "true" : "false";
    case Type::Integer:
        return QByteArray::number(token.data.toLongLong());
    case Type::Real:
        return numeric(token.data.toDouble());
    case Type::String:
        return "<" + token.data.toByteArray().toHex() + ">";
    case Type::Name:
        return PDFDocumentWriter::getSerializedObject(
            PDFObject::createName(token.data.toByteArray()));
    case Type::ArrayStart:
        return "[";
    case Type::ArrayEnd:
        return "]";
    case Type::Null:
        return "null";
    default:
        fail("この内容命令の安全な墨消しにはまだ対応していません。");
    }
}
QPainterPath strokeBoundsPath(const QPainterPath& path, const PDFPageContentProcessorState* state)
{
    QPainterPathStroker stroker;
    stroker.setWidth(state->getLineWidth());
    stroker.setCapStyle(state->getLineCapStyle());
    stroker.setJoinStyle(state->getLineJoinStyle());
    stroker.setMiterLimit(state->getMitterLimit());
    QPainterPath result;
    for (int start = 0; start < path.elementCount();)
    {
        int end = start + 1;
        while (end < path.elementCount() && !path.elementAt(end).isMoveTo())
            ++end;
        const auto first = path.elementAt(start);
        const auto last = path.elementAt(end - 1);
        QPainterPath part(QPointF(first.x, first.y));
        for (int i = start + 1; i < end; ++i)
        {
            const auto element = path.elementAt(i);
            if (element.isLineTo())
                part.lineTo(element.x, element.y);
            else if (element.isCurveTo() && i + 2 < end)
            {
                const auto control = path.elementAt(i + 1), finish = path.elementAt(i + 2);
                part.cubicTo(element.x, element.y, control.x, control.y, finish.x, finish.y);
                i += 2;
            }
            else
                fail("図形のパス構造を確認できません。");
        }
        const bool closed = first.x == last.x && first.y == last.y;
        // Qt's stroker produces long clamped miters for a closed two-edge
        // out-and-back segment. PDF renderers bevel that reversing join; a
        // round join instead has the envelope of a round-capped open segment.
        if (closed && end - start == 3 && path.elementAt(start + 1).isLineTo() && last.isLineTo())
        {
            const auto middle = path.elementAt(start + 1);
            part = QPainterPath(QPointF(first.x, first.y));
            part.lineTo(middle.x, middle.y);
            stroker.setCapStyle(state->getLineJoinStyle() == Qt::RoundJoin ? Qt::RoundCap
                                                                           : Qt::FlatCap);
            result = result.united(stroker.createStroke(part));
            stroker.setCapStyle(state->getLineCapStyle());
        }
        else
        {
            if (closed)
                part.closeSubpath();
            result = result.united(stroker.createStroke(part));
        }
        start = end;
    }
    return result;
}
class ContentProcessor final : public PDFPageContentProcessor
{
    struct FontResource
    {
        QByteArray name;
        PDFObject declaration;
    };
    struct TextItem
    {
        CID cid = 0;
        QChar character;
        double width = 0, advance = 0, wordSpacing = 0;
        bool glyph = false, selected = false;
        QByteArray sourceCode;
        int sourceString = -1;
    };
    struct TextOperation
    {
        PDFFontPointer font;
        QByteArray resourceName;
        QVector<TextItem> items;
        double size = 0, spacing = 0, wordSpacing = 0;
        double sourceWordSpacing = 0, horizontalScale = 1;
        QTransform sourceMatrix;
        bool identity = true;
        QByteArray prefix;
        QByteArray originalCommand;
        std::optional<double> blankAnchorWidth;
        qsizetype nextGlyph = 0;
    };
    struct Operation
    {
        QByteArray bytes;
        std::shared_ptr<TextOperation> text;
    };

public:
    ContentProcessor(const PDFPage* page, const PDFDocument* document, const PDFFontCache* fonts,
                     const PDFCMS* cms, QPainterPath regions, PDFDocumentBuilder& builder,
                     std::function<bool()> cancelled,
                     const RedactionResourceUsage* shared = nullptr,
                     RedactionFontCvtUsage* cvtUsage = nullptr)
        : PDFPageContentProcessor(page, document, fonts, cms, nullptr, {}, {}),
          regions(std::move(regions)), builder(builder), cancelled(std::move(cancelled)),
          shared(shared), cvtUsage(cvtUsage)
    {
        pageClip = page->getCropBox();
        result.bytes = "q\n";
    }
    RedactedPageContent finish()
    {
        if (deferredError)
            std::rethrow_exception(deferredError);
        if (!analysisError.isEmpty())
            fail("墨消し前の内容解析に失敗しました: " + analysisError);
        stop();
        if (graphicsDepth != 0)
            fail("描画状態の対応が不正です。墨消ししていません。");
        if (!regions.isEmpty() && !pathOperations.isEmpty())
            fail("未描画パスの安全な墨消しは未評価です。");
        if (pendingClip)
            fail("クリップの未描画終了を確認できません。墨消ししていません。");
        QMap<QByteArray, QMap<CID, int>> recoded;
        QSet<QByteArray> rawFonts;
        for (const auto& operation : operations)
            if (operation.text && !operation.text->identity &&
                operation.text->sourceWordSpacing != 0)
                rawFonts.insert(operation.text->resourceName);
        for (const auto& operation : operations)
            if (operation.text && rawFonts.contains(operation.text->resourceName))
                validateRawFrame(*operation.text);
        if (shared)
            for (size_t i = 0; i < result.fonts.getCount(); ++i)
                if (shared->affectedFont(result.fonts.getValue(i)))
                    selectedFontsNeedingCleanup.insert(result.fonts.getKey(i).getString());
        for (const auto& name : selectedFontsNeedingCleanup)
        {
            const auto original = result.fonts.get(name);
            if (const auto clean = canonicalRedactionStandardFont(*getDocument(), original))
            {
                rememberRemovedDependencies(original);
                if (original.isDictionary())
                    mergeRemovedDirectRedactionFonts(result.removedDirectFontDeclarations,
                                                     {original}, cancelled);
                result.fonts.setEntry(
                    PDFInplaceOrMemoryString(name),
                    PDFObject::createReference(builder.addObject(detail::dictObject(*clean))));
                continue;
            }
            if (!original.isReference())
                fail("直接格納されたサブセット書体の共有情報を確認できません。");
            rememberRemovedDependencies(original);
            QVector<RedactionGlyph> glyphs;
            QMap<QByteArray, RedactionCodeRoute> retainedCodes;
            PDFFontPointer font;
            std::optional<RedactionFontBounds> bounds;
            for (const auto& operation : operations)
                if (operation.text && operation.text->resourceName == name)
                {
                    font = operation.text->font;
                    const auto profile =
                        font->getFontType() == FontType::Type0 && !operation.text->identity
                            ? RedactionFontBounds::EmbeddedCMapSource
                            : RedactionFontBounds::RetainedGlyphs;
                    if (bounds && *bounds != profile)
                        fail("同じ書体資源のCMap描画条件が一致しません。墨消ししていません。");
                    bounds = profile;
                    for (const auto& item : operation.text->items)
                        if (item.glyph && !item.selected)
                        {
                            if (!recoded[name].contains(item.cid))
                            {
                                quint32 gid = 0;
                                if (const auto simple =
                                        dynamic_cast<const PDFSimpleFont*>(font.data()))
                                    gid = simple->getGlyphIndices()->at(item.cid);
                                else if (const auto composite =
                                             dynamic_cast<const PDFType0Font*>(font.data()))
                                    gid = composite->getCIDtoGIDMapper()->tryMap(item.cid).value_or(
                                        0);
                                if (!gid || gid > 65535 || glyphs.size() >= 8191)
                                    fail("残る文字の字形対応を安全に確認できません。");
                                recoded[name][item.cid] = glyphs.size() + 1;
                                glyphs << RedactionGlyph{quint16(gid), item.character, item.width};
                            }
                            if (rawFonts.contains(name))
                            {
                                const RedactionCodeRoute route{
                                    item.sourceCode, recoded[name][item.cid], item.character};
                                if ((route.code.size() != 1 && route.code.size() != 2) ||
                                    (retainedCodes.contains(route.code) &&
                                     (retainedCodes[route.code].subsetCid != route.subsetCid ||
                                      retainedCodes[route.code].character != route.character)))
                                    fail("保持コードと書体資源の対応を確認できません。");
                                retainedCodes.insert(route.code, route);
                            }
                        }
                }
            if (!glyphs.isEmpty())
            {
                result.fonts.setEntry(
                    PDFInplaceOrMemoryString(name),
                    PDFObject::createReference(builder.addObject(rebuildRedactionFont(
                        *font->getFontDescriptor(), glyphs, builder, cancelled, cvtUsage,
                        bounds.value_or(RedactionFontBounds::RetainedGlyphs),
                        retainedCodes.values()))));
                continue;
            }
            PDFDictionary stub;
            detail::set(stub, "Type", PDFObject::createName("Font"));
            detail::set(stub, "Subtype", PDFObject::createName("Type1"));
            detail::set(stub, "BaseFont", PDFObject::createName("Helvetica"));
            // Only numeric TJ advances survive for this resource. Their value
            // depends on font size and scale, never on glyph widths. A valid
            // standard font preserves that state without retaining the subset.
            result.fonts.setEntry(
                PDFInplaceOrMemoryString(name),
                PDFObject::createReference(builder.addObject(detail::dictObject(stub))));
        }
        for (const auto& operation : operations)
        {
            stop();
            result.bytes += operation.text && (operation.bytes.isEmpty() ||
                                               recoded.contains(operation.text->resourceName))
                                ? serializeText(*operation.text, recoded, rawFonts)
                                : operation.bytes;
            if (result.bytes.size() > 64 * 1024 * 1024)
                fail("墨消し候補の内容が処理上限を超えています。");
        }
        result.bytes += "Q\nq\n0 g\n";
        for (const auto& rectangle : rectangles)
            result.bytes += numeric(rectangle.x()) + " " + numeric(rectangle.y()) + " " +
                            numeric(rectangle.width()) + " " + numeric(rectangle.height()) +
                            " re f\n";
        result.bytes += "Q\n";
        return std::move(result);
    }
    QVector<QRectF> rectangles;

protected:
    void reportRenderError(RenderErrorType type, QString message) override
    {
        if (type != RenderErrorType::Information)
            analysisError = message;
        PDFPageContentProcessor::reportRenderError(type, std::move(message));
    }
    void performInterceptInstruction(Operator op, ProcessOrder order,
                                     const QByteArray& name) override
    {
        if (order == ProcessOrder::AfterOperation)
        {
            // Upstream invokes this hook from a noexcept QScopeGuard destructor.
            // Defer errors until the next normal hook or finish(), and never
            // throw a second exception while the original operation unwinds.
            if (std::uncaught_exceptions())
                return;
            try
            {
                processInstruction(op, order, name);
            }
            catch (...)
            {
                deferredError = std::current_exception();
            }
            return;
        }
        if (deferredError)
            std::rethrow_exception(deferredError);
        processInstruction(op, order, name);
    }
    void processInstruction(Operator op, ProcessOrder order, const QByteArray& name)
    {
        stop();
        if (order == ProcessOrder::BeforeOperation)
        {
            if (op == Operator::Invalid || name == "BI" || name == "ID" || name == "EI" ||
                name == "sh" || name == "BMC" || name == "BDC" || name == "EMC" || name == "MP" ||
                name == "DP" || name == "BX" || name == "EX")
                fail("この内容命令を含むPDFの墨消しにはまだ対応していません: " +
                     QString::fromLatin1(name));
            command = name;
            serialized.clear();
            selectedCharacters = 0;
            textOperation.reset();
            suppressForm = removeForm = false;
            replacementImage.clear();
            removePath = false;
            finishClip = false;
            if (pendingClip)
            {
                if (name != "n")
                    fail("矩形クリップの後は未描画終了が必要です。墨消ししていません。");
                finishClip = true;
            }
            if (!regions.isEmpty() && (name == "W" || name == "W*"))
            {
                if (pathOperations.size() != 1 || !rectanglePath ||
                    pathOperations.first() != operations.size() - 1)
                    fail("単一の矩形以外のクリップは安全な墨消しを確認できません。");
                clipValidated = false;
            }
            if (!regions.isEmpty() && !pathOperations.isEmpty() &&
                (name == "cm" || name == "q" || name == "Q" || name == "BT" || name == "Do" ||
                 (name == "n" && !finishClip)))
                fail("未描画パスやパス構築中の座標系変更の墨消しは未評価です。");
            if (name == "cs" || name == "CS")
            {
                if (getOperands().size() != 1 ||
                    getOperands()[0].type != PDFLexicalAnalyzer::TokenType::Name)
                    fail("色空間の命令が不正です。");
                const auto key = getOperands()[0].data.toByteArray();
                auto space = PDFObject::createName(key);
                if (getColorSpaceDictionary() && getColorSpaceDictionary()->hasKey(key))
                    space = getDocument()->getObject(getColorSpaceDictionary()->get(key));
                if (!space.isName() ||
                    (space.getString() != "DeviceRGB" && space.getString() != "DeviceGray" &&
                     space.getString() != "DeviceCMYK"))
                    fail("この色空間の安全な墨消しは未評価です。");
                result.colorSpaces.setEntry(PDFInplaceOrMemoryString(key), PDFObject(space));
            }
            if (name == "scn" || name == "SCN")
                for (size_t i = 0; i < getOperands().size(); ++i)
                    if (getOperands()[i].type != PDFLexicalAnalyzer::TokenType::Integer &&
                        getOperands()[i].type != PDFLexicalAnalyzer::TokenType::Real)
                        fail("パターンを使う色の安全な墨消しは未評価です。");
            if (name == "gs")
            {
                if (getOperands().size() != 1 || !getExtendedGraphicStateDictionary() ||
                    getOperands()[0].type != PDFLexicalAnalyzer::TokenType::Name)
                    fail("拡張描画状態の命令が不正です。");
                const auto key = getOperands()[0].data.toByteArray();
                const auto state = getDocument()->getDictionaryFromObject(
                    getExtendedGraphicStateDictionary()->get(key));
                if (!state)
                    fail("拡張描画状態の参照が不正です。");
                PDFDictionary sanitized;
                for (size_t i = 0; i < state->getCount(); ++i)
                {
                    const auto name = state->getKey(i).getString();
                    const auto value = getDocument()->getObject(state->getValue(i));
                    const bool valid =
                        (name == "Type" && value == PDFObject::createName("ExtGState")) ||
                        ((name == "ca" || name == "CA") && number(value) == 1) ||
                        (name == "SM" && number(value) >= 0 && number(value) <= 1) ||
                        (name == "SA" && value.isBool()) ||
                        (name == "AIS" && value == PDFObject::createBool(false)) ||
                        (name == "SMask" && value == PDFObject::createName("None")) ||
                        (name == "BM" && value == PDFObject::createName("Normal"));
                    if (!valid)
                        fail("透明合成や未知属性を持つ描画状態の墨消しは未評価です。");
                    sanitized.setEntry(PDFInplaceOrMemoryString(name), PDFObject(value));
                }
                result.graphicsStates.setEntry(PDFInplaceOrMemoryString(key),
                                               detail::dictObject(sanitized));
            }
            if (name == "q")
            {
                fontStack.append(activeFont);
                ++graphicsDepth;
            }
            if (name == "Q" && --graphicsDepth < 0)
                fail("描画状態の対応が不正です。");
            if (name == "Q")
                activeFont = fontStack.takeLast();
            for (size_t i = 0; i < getOperands().size(); ++i)
                serialized += tokenBytes(getOperands()[i]) + " ";
            if (name == "Tf")
            {
                if (getOperands().size() != 2 || !getFontDictionary() ||
                    getOperands()[0].type != PDFLexicalAnalyzer::TokenType::Name)
                    fail("書体の参照が不正です。");
                const auto fontName = getOperands()[0].data.toByteArray();
                activeFont = {fontName, PDFObject(getFontDictionary()->get(fontName))};
                result.fonts.setEntry(PDFInplaceOrMemoryString(fontName),
                                      PDFObject(getFontDictionary()->get(fontName)));
            }
            if (name == "Tj" || name == "TJ" || name == "'" || name == "\"")
                validateCustomTextOperands(name);
            if (name == "Do")
            {
                if (getOperands().size() != 1)
                    fail("画像命令が不正です。");
                imageName = getOperands()[0].data.toByteArray();
                const auto objects = getXObjectDictionary();
                const auto object =
                    objects ? getDocument()->getObject(objects->get(imageName)) : PDFObject();
                if (!object.isStream())
                    fail("画像参照が不正です。");
                const auto dictionary = object.getStream()->getDictionary();
                const auto subtype = getDocument()->getObject(dictionary->get("Subtype"));
                if (subtype.isName() && subtype.getString() == "Form")
                {
                    prepareForm(dictionary, objects->get(imageName));
                    return;
                }
                if (!subtype.isName() || subtype.getString() != "Image" ||
                    dictionary->hasKey("ImageMask") || dictionary->hasKey("Mask") ||
                    dictionary->hasKey("SMask") || dictionary->hasKey("OC") ||
                    dictionary->hasKey("Alternates"))
                    fail("Form "
                         "XObject、画像マスク、レイヤー等の安全な墨消しにはまだ対応していません。");
            }
        }
        else
        {
            if (finishClip)
            {
                for (const auto index : pathOperations)
                    operations[index].bytes.clear();
                operations[clipOperation].bytes.clear();
                pendingClip = false;
                // Removing a redundant clip is not removal of selected content.
            }
            else if (removePath)
            {
                for (const auto index : pathOperations)
                    operations[index].bytes.clear();
                ++result.removedContentGroups;
            }
            else if (removeForm)
                ++result.removedContentGroups;
            else if (selectedCharacters)
            {
                if (!textOperation ||
                    (command != "Tj" && command != "TJ" && command != "'" && command != "\""))
                    fail("文字命令の一部だけの墨消しにはまだ対応していません。出力していません。");
                if (requiresFontCleanup())
                    selectedFontsNeedingCleanup.insert(activeFont.name);
                operations.push_back({{}, textOperation});
                ++result.removedTextSegments;
            }
            else if (!replacementImage.isEmpty())
                operations.push_back({PDFDocumentWriter::getSerializedObject(
                                          PDFObject::createName(replacementImage)) +
                                          " Do\n",
                                      {}});
            else
            {
                operations.push_back({serialized + command + "\n", textOperation});
                if (command == "Do")
                    result.xobjects.setEntry(PDFInplaceOrMemoryString(imageName),
                                             PDFObject(getXObjectDictionary()->get(imageName)));
            }
            if (operations.size() > 1000000)
                fail("墨消し候補の内容が処理上限を超えています。");
            if (command == "m" || command == "l" || command == "c" || command == "v" ||
                command == "y" || command == "h" || command == "re")
            {
                pathOperations.push_back(operations.size() - 1);
                rectanglePath = command == "re" && pathOperations.size() == 1;
            }
            if (!regions.isEmpty() && (command == "W" || command == "W*"))
            {
                if (!clipValidated)
                    fail("クリップの表示領域を確認できません。墨消ししていません。");
                pendingClip = true;
                clipOperation = operations.size() - 1;
            }
            if (command == "S" || command == "s" || command == "f" || command == "F" ||
                command == "f*" || command == "B" || command == "B*" || command == "b" ||
                command == "b*" || command == "n")
            {
                pathOperations.clear();
                rectanglePath = false;
            }
        }
    }
    void performClipping(const QPainterPath& path, Qt::FillRule) override
    {
        if (regions.isEmpty() || (command != "W" && command != "W*"))
            return;
        const auto transform = getGraphicState()->getCurrentTransformationMatrix();
        const bool aligned = (transform.m12() == 0 && transform.m21() == 0) ||
                             (transform.m11() == 0 && transform.m22() == 0);
        const auto bounds = transform.map(path).boundingRect();
        if (!aligned || !transform.isInvertible() || path.elementCount() != 5 ||
            pageClip.isEmpty() || bounds.isEmpty() || !std::isfinite(bounds.x()) ||
            !std::isfinite(bounds.y()) || !std::isfinite(bounds.width()) ||
            !std::isfinite(bounds.height()) || !bounds.contains(pageClip))
            fail("表示領域を狭めるクリップや傾いたクリップの墨消しは未評価です。");
        clipValidated = true;
    }
    void performProcessTextSequence(const TextSequence& sequence, ProcessOrder order) override
    {
        if (order != ProcessOrder::BeforeOperation)
            return;
        const auto font = getGraphicState()->getTextFont();
        if (font && !getFontDictionary())
            fail("継承した書体の局所資源辞書を確認できません。墨消ししていません。");
        if (font && getFontDictionary())
        {
            const auto original = getFontDictionary()->get(activeFont.name);
            if (activeFont.name.isEmpty() || original != activeFont.declaration ||
                (original.isReference() &&
                 getFontCache()->getFont(original, activeFont.name) != font))
                fail("文字命令の書体資源名と描画状態が一致しません。");
            result.fonts.setEntry(PDFInplaceOrMemoryString(activeFont.name), PDFObject(original));
        }
        const auto mode = getGraphicState()->getTextRenderingMode();
        if (mode != TextRenderingMode::Fill && mode != TextRenderingMode::Invisible)
            fail("輪郭線やクリッピングを使った文字の安全な墨消しは未評価です。");
        if (!font || font->getFontType() == FontType::Type3)
            fail("この書体の安全な墨消しにはまだ対応していません。");
        const auto realized =
            getFontCache()->getRealizedFont(font, getGraphicState()->getTextFontSize(), this);
        if (!realized || !realized->isHorizontalWritingSystem())
            fail("この書体や縦書きの安全な墨消しにはまだ対応していません。");
        for (const auto& item : sequence.items)
            if ((item.glyph && item.character.isNull()) || (!item.glyph && item.cid))
                fail("文字対応を検証できないため墨消ししていません。");
        const auto state = getGraphicState();
        if (state->getTextFontSize() <= 0)
            fail("ゼロや負の文字サイズの墨消しは未評価です。");
        textOperation = std::make_shared<TextOperation>();
        textOperation->font = font;
        textOperation->resourceName = activeFont.name;
        textOperation->size = state->getTextFontSize();
        textOperation->spacing = state->getTextCharacterSpacing();
        textOperation->sourceWordSpacing = state->getTextWordSpacing();
        textOperation->horizontalScale = state->getTextHorizontalScaling() * .01;
        textOperation->sourceMatrix =
            state->getTextMatrix() * state->getCurrentTransformationMatrix();
        textOperation->originalCommand = serialized + command + "\n";
        const auto matrix = state->getTextMatrix();
        const auto declaration =
            getDocument()->getDictionaryFromObject(getFontDictionary()->get(activeFont.name));
        if (!declaration)
            fail("文字の書体宣言を確認できません。");
        const auto encoding = getDocument()->getObject(declaration->get("Encoding"));
        if (font->getFontType() == FontType::Type1 && matrix.m11() == 1 && matrix.m22() == 1 &&
            matrix.m12() == 0 && matrix.m21() == 0 && state->getTextHorizontalScaling() == 100 &&
            state->getTextCharacterSpacing() == 0 && state->getTextWordSpacing() == 0 &&
            canonicalRedactionStandardFont(*getDocument(),
                                           getFontDictionary()->get(activeFont.name)))
            textOperation->blankAnchorWidth = glyphWidth(32);
        // PDF Tw affects only the single-byte code 32, never Identity-H codes.
        textOperation->wordSpacing =
            font->getFontType() == FontType::Type0 ? 0 : state->getTextWordSpacing();
        QVector<RedactionCMap::Code> sourceCodes;
        QVector<int> sourceStrings;
        if (font->getFontType() == FontType::Type0)
        {
            textOperation->identity = encoding.isName() && encoding.getString() == "Identity-H";
            if (!textOperation->identity)
            {
                const auto map = customMap(*declaration);
                validateMixedWordSpacing(*map, state->getTextWordSpacing(),
                                         state->getTextCharacterSpacing());
                int sourceString = 0;
                for (size_t index = 0; index < getOperands().size(); ++index)
                {
                    const auto& operand = getOperands()[index];
                    if (operand.type == PDFLexicalAnalyzer::TokenType::String)
                    {
                        const auto codes = map->decode(operand.data.toByteArray());
                        sourceCodes += codes;
                        for (qsizetype at = 0; at < codes.size(); ++at)
                            sourceStrings.append(sourceString);
                        ++sourceString;
                    }
                }
                qsizetype at = 0;
                for (const auto& item : sequence.items)
                    if (item.glyph)
                    {
                        if (at >= sourceCodes.size() || sourceCodes[at].cid != item.cid ||
                            sourceCodes[at].character != item.character)
                            fail("CMapと字形解析の文字対応が一致しません。");
                        const auto& source = sourceCodes[at];
                        const bool applies =
                            source.sourceByteCount == 1 && source.sourceValue == 32;
                        if (!item.sourceWordSpacingApplies.has_value() ||
                            *item.sourceWordSpacingApplies != applies)
                            fail("CMapの生コードと描画の単語間隔状態が一致しません。");
                        ++at;
                    }
                if (at != sourceCodes.size())
                    fail("CMapの文字列を全て検証できません。");
            }
        }
        if (command == "'")
            textOperation->prefix = "T*\n";
        else if (command == "\"")
            textOperation->prefix = numeric(state->getTextWordSpacing()) + " Tw\n" +
                                    numeric(state->getTextCharacterSpacing()) + " Tc\nT*\n";
        qsizetype sourceIndex = 0;
        for (const auto& item : sequence.items)
        {
            double wordSpacing = item.character == QChar(' ') ? textOperation->wordSpacing : 0;
            QByteArray sourceCode;
            int sourceString = -1;
            if (item.glyph && !sourceCodes.isEmpty())
            {
                const auto& source = sourceCodes[sourceIndex];
                sourceString = sourceStrings[sourceIndex++];
                if (source.sourceByteCount == 2)
                    sourceCode.append(char(source.sourceValue >> 8));
                sourceCode.append(char(source.sourceValue));
                wordSpacing = source.sourceByteCount == 1 && source.sourceValue == 32
                                  ? state->getTextWordSpacing()
                                  : 0;
            }
            const double width = item.glyph ? glyphWidth(item.cid) : 0;
            if (item.glyph && !sourceCodes.isEmpty() && state->getTextWordSpacing() != 0)
            {
                const double effectiveAdvance =
                    width * textOperation->size / 1000 + textOperation->spacing + wordSpacing;
                if (!std::isfinite(effectiveAdvance) || effectiveAdvance <= 0)
                    fail("非0の単語間隔とゼロや逆方向の文字移動の併用は未評価です。"
                         "墨消し候補は保存していません。");
            }
            textOperation->items << TextItem{
                item.cid,         item.character, width,      double(item.advance), wordSpacing,
                bool(item.glyph), false,          sourceCode, sourceString};
        }
    }
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        return (kind == ContentKind::Forms && suppressForm) ||
               PDFPageContentProcessor::isContentKindSuppressed(kind);
    }
    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        if (!textOperation)
            fail("墨消し対象の文字命令を確認できません。");
        auto& items = textOperation->items;
        while (textOperation->nextGlyph < items.size() && !items[textOperation->nextGlyph].glyph)
            ++textOperation->nextGlyph;
        if (textOperation->nextGlyph >= items.size() ||
            items[textOperation->nextGlyph].character != info.character)
            fail("文字列と字形の対応が一致しません。");
        auto& item = items[textOperation->nextGlyph++];
        if (info.character.isSpace())
        {
            if (info.character == QChar(' ') && info.outline.isEmpty() &&
                textOperation->font->getFontType() == FontType::Type0 && !textOperation->identity)
            {
                // An empty space has no ink bbox. Its origin can fall inside
                // selected neighboring ink while the complete advance escapes.
                const double advance = info.advance + textOperation->spacing + item.wordSpacing;
                if (!std::isfinite(advance))
                    fail("空白文字の移動量が不正です。墨消ししていません。");
                item.selected = coversAdvanceSegment(info.matrix.map(QPointF()),
                                                     info.matrix.map(QPointF(advance, 0)));
            }
            else
                item.selected = regions.contains(info.matrix.map(QPointF()));
            if (item.selected)
                ++selectedCharacters;
            return;
        }
        auto bounds = info.matrix.map(info.outline).boundingRect();
        if (bounds.isEmpty())
            fail("文字の範囲を確認できません。");
        if (regions.intersects(bounds))
        {
            if (!covers(bounds))
                fail("文字の一部を横切る墨消しにはまだ対応していません。");
            ++selectedCharacters;
            item.selected = true;
        }
    }
    void performPathPainting(const QPainterPath& path, bool stroke, bool fill, bool text,
                             Qt::FillRule) override
    {
        if (regions.isEmpty() || text || (!stroke && !fill))
            return;
        const auto state = getGraphicState();
        auto painted = fill ? path : QPainterPath();
        if (stroke)
        {
            if (state->getLineWidth() <= 0 || !std::isfinite(state->getLineWidth()))
                fail("幅0や不正な線の安全な墨消しは未評価です。");
            // The solid outline conservatively contains all dash segments.
            painted = painted.united(strokeBoundsPath(path, state));
        }
        const auto bounds = state->getCurrentTransformationMatrix().map(painted).boundingRect();
        if (!std::isfinite(bounds.x()) || !std::isfinite(bounds.y()) ||
            !std::isfinite(bounds.width()) || !std::isfinite(bounds.height()))
            fail("図形の範囲が不正です。");
        if (regions.intersects(bounds))
        {
            if (!covers(bounds) || pathOperations.isEmpty())
                fail(QString("図形や罫線の一部を横切る墨消しにはまだ対応していません。描画範囲: "
                             "%1, %2, %3, %4 pt")
                         .arg(bounds.x(), 0, 'f', 4)
                         .arg(bounds.y(), 0, 'f', 4)
                         .arg(bounds.width(), 0, 'f', 4)
                         .arg(bounds.height(), 0, 'f', 4));
            removePath = true;
        }
    }
    bool performOriginalImagePainting(const PDFImage&, const PDFStream*,
                                      PDFObjectReference) override
    {
        return false;
    }
    void performImagePainting(const QImage& source) override
    {
        const auto matrix = getGraphicState()->getCurrentTransformationMatrix();
        if (!regions.intersects(matrix.mapRect(QRectF(0, 0, 1, 1))))
            return;
        const auto original = getXObjectDictionary()->get(imageName);
        if (!original.isReference())
            fail("直接格納された画像の共有経路を検証できません。墨消ししていません。");
        result.modifiedImageSources.insert(original.getReference());
        if (source.isNull() || qint64(source.width()) * source.height() > 40000000)
            fail("画像サイズが墨消し処理の上限を超えています。");
        bool invertible = false;
        const QTransform pixelsToUnit(1.0 / source.width(), 0, 0, -1.0 / source.height(), 0, 1);
        const auto inverse = (pixelsToUnit * matrix).inverted(&invertible);
        if (!invertible)
            fail("画像の座標を安全に変換できません。");
        const auto mask = inverse.map(regions);
        const auto limit = mask.boundingRect().toAlignedRect().intersected(source.rect());
        auto image = source.convertToFormat(QImage::Format_RGB32);
        if (covers(matrix.mapRect(QRectF(0, 0, 1, 1))))
            image.fill(qRgb(0, 0, 0));
        else
            for (int y = limit.top(); y <= limit.bottom(); ++y)
            {
                stop();
                for (int x = limit.left(); x <= limit.right(); ++x)
                    if (mask.intersects(QRectF(x, y, 1, 1)))
                    {
                        // A partly covered, already white pixel contains no
                        // confidential ink. Keep that known empty boundary;
                        // the final vector fill supplies the exact black edge.
                        // Painting the whole coarse pixel black would extend
                        // the visible redaction outside the requested region.
                        if (image.pixel(x, y) != qRgb(255, 255, 255) ||
                            mask.contains(QRectF(x, y, 1, 1)))
                            image.setPixel(x, y, qRgb(0, 0, 0));
                    }
            }
        int suffix = result.modifiedImages + 1;
        const auto objects = getXObjectDictionary();
        do
            replacementImage = "TatsujinRedactImage" + QByteArray::number(suffix++);
        while ((objects && objects->hasKey(replacementImage)) ||
               result.xobjects.hasKey(replacementImage));
        result.xobjects.setEntry(
            PDFInplaceOrMemoryString(replacementImage),
            PDFObject::createReference(embedImage(builder, image, ImagePrediction::None)));
        ++result.modifiedImages;
    }

private:
    std::shared_ptr<RedactionCMap> customMap(const PDFDictionary& declaration)
    {
        const auto name = activeFont.name;
        if (!customMaps.contains(name))
            customMaps[name] =
                std::make_shared<RedactionCMap>(*getDocument(), declaration, cancelled);
        return customMaps[name];
    }
    void validateMixedWordSpacing(const RedactionCMap& map, double wordSpacing,
                                  double characterSpacing) const
    {
        if (wordSpacing == 0)
            return;
        const auto state = getGraphicState();
        const auto matrix = state->getTextMatrix();
        if (!std::isfinite(wordSpacing) || !std::isfinite(characterSpacing) ||
            !std::isfinite(state->getTextHorizontalScaling()) ||
            state->getTextHorizontalScaling() <= 0 || !map.hasSingleByteCodes() ||
            map.hasNumericCodeLengthCollision() || map.hasTwoByteCode32() || matrix.m11() != 1 ||
            matrix.m22() != 1 || matrix.m12() != 0 || matrix.m21() != 0)
            fail("このCMapまたは文字行列と非0の単語間隔の併用は未評価です。"
                 "墨消し候補は保存していません。");
    }
    void validateCustomTextOperands(const QByteArray& name)
    {
        const auto state = getGraphicState();
        const auto font = state->getTextFont();
        if (!font || font->getFontType() != FontType::Type0)
            return;
        if (!getFontDictionary())
            fail("継承した書体の局所資源辞書を確認できません。墨消ししていません。");
        const auto original = getFontDictionary()->get(activeFont.name);
        if (activeFont.name.isEmpty() || original != activeFont.declaration ||
            (original.isReference() && getFontCache()->getFont(original, activeFont.name) != font))
            fail("文字命令の書体資源名と描画状態が一致しません。");
        const auto declaration = getDocument()->getDictionaryFromObject(original);
        if (!declaration)
            fail("CMapの字体宣言を確認できません。");
        const auto encoding = getDocument()->getObject(declaration->get("Encoding"));
        if (encoding.isName() && encoding.getString() == "Identity-H")
            return;
        const auto map = customMap(*declaration);
        double wordSpacing = state->getTextWordSpacing();
        double characterSpacing = state->getTextCharacterSpacing();
        if (name == "\"")
        {
            const auto& operands = getOperands();
            if (operands.size() != 3 ||
                (operands[0].type != PDFLexicalAnalyzer::TokenType::Integer &&
                 operands[0].type != PDFLexicalAnalyzer::TokenType::Real) ||
                (operands[1].type != PDFLexicalAnalyzer::TokenType::Integer &&
                 operands[1].type != PDFLexicalAnalyzer::TokenType::Real))
                fail("単語間隔を設定する文字命令が不正です。墨消ししていません。");
            wordSpacing = operands[0].data.toDouble();
            characterSpacing = operands[1].data.toDouble();
        }
        // Empty text bypasses the engine's text-sequence hook. Validate its
        // effective state here, including Tw assigned by the double quote.
        validateMixedWordSpacing(*map, wordSpacing, characterSpacing);
        for (size_t index = 0; index < getOperands().size(); ++index)
        {
            const auto& operand = getOperands()[index];
            if (operand.type == PDFLexicalAnalyzer::TokenType::String)
                map->validate(operand.data.toByteArray());
        }
    }
    void validateRawFrame(const TextOperation& operation) const
    {
        const auto first = std::find_if(operation.items.cbegin(), operation.items.cend(),
                                        [](const TextItem& item) { return item.glyph; });
        if (first == operation.items.cend())
            return;
        const auto last = std::find_if(operation.items.crbegin(), operation.items.crend(),
                                       [](const TextItem& item) { return item.glyph; });
        // Leading/trailing deletion changes the external/local float cursor
        // frame. The evaluated raw-code unit contains internal deletions only.
        if (first->selected || last->selected)
            fail("非0単語間隔の書体で先頭や末尾の字形を除く位置再現は未評価です。"
                 "墨消し候補は保存していません。");
    }
    QByteArray serializeRawText(const TextOperation& operation) const
    {
        const auto fontBinding =
            PDFDocumentWriter::getSerializedObject(PDFObject::createName(operation.resourceName)) +
            " " + numeric(operation.size) + " Tf\n";
        if (std::none_of(operation.items.cbegin(), operation.items.cend(),
                         [](const TextItem& item) { return item.glyph; }))
            return fontBinding + operation.originalCommand;
        QByteArray bytes = fontBinding + operation.prefix + "[", retained;
        float sourceCursor = 0, emittedCursor = 0;
        double sourceExact = 0, emittedExact = 0;
        int retainedString = -1;
        bool removing = false, seenGlyph = false;
        const float size = float(operation.size), spacing = float(operation.spacing);
        if (!std::isfinite(size) || size <= 0 || !std::isfinite(spacing))
            fail("生コードの文字間隔の数値を確認できません。");
        auto auditPosition = [&]
        {
            const auto delta = (emittedExact - sourceExact) * operation.horizontalScale;
            const auto error = std::hypot(delta * operation.sourceMatrix.m11(),
                                          delta * operation.sourceMatrix.m12());
            if (!std::isfinite(error) || error > .5 || !std::isfinite(sourceCursor) ||
                !std::isfinite(emittedCursor))
                fail("生コードの保持位置を必要な精度で再現できません。");
        };
        auto flushText = [&]
        {
            if (!retained.isEmpty())
            {
                bytes += "<" + retained.toHex() + "> ";
                retained.clear();
            }
        };
        auto reconcile = [&]
        {
            flushText();
            if (sourceCursor != emittedCursor)
            {
                const auto value =
                    redactionStandardTextAdjustment(emittedCursor, sourceCursor, size, cancelled);
                if (!value)
                    fail("生コードの除去区間の移動を正確に再現できません。");
                const auto encoded = numeric(double(*value));
                bool valid = false;
                const double actual = encoded.toDouble(&valid);
                if (!valid || float(actual) != *value)
                    fail("生コードの移動数値を正確に保存できません。");
                bytes += encoded + " ";
                emittedCursor -= *value * size / 1000;
                emittedExact -= actual * operation.size / 1000;
            }
            if (sourceCursor != emittedCursor)
                fail("生コードの文字原点を正確に再現できません。");
            removing = false;
            auditPosition();
        };
        for (qsizetype at = 0; at < operation.items.size();)
        {
            stop();
            if (!operation.items[at].glyph)
            {
                // PDFium groups adjacent numbers (empty strings skipped) into
                // one float kerning. Leading/trailing kerning stays external.
                float grouped = 0;
                double exact = 0;
                QByteArray operands;
                do
                {
                    const auto value = operation.items[at++].advance;
                    const auto encoded = numeric(value);
                    bool valid = false;
                    const auto actual = encoded.toDouble(&valid);
                    if (!valid || float(actual) != float(value))
                        fail("生コードの元TJ数値を正確に保存できません。");
                    grouped += float(value);
                    exact += value;
                    operands += encoded + " ";
                } while (at < operation.items.size() && !operation.items[at].glyph);
                if (!seenGlyph || at == operation.items.size())
                {
                    flushText();
                    bytes += operands;
                    continue;
                }
                sourceCursor -= grouped * size / 1000;
                sourceExact -= exact * operation.size / 1000;
                if (!removing)
                {
                    flushText();
                    bytes += operands;
                    emittedCursor -= grouped * size / 1000;
                    emittedExact -= exact * operation.size / 1000;
                }
                continue;
            }
            const auto& item = operation.items[at++];
            if (item.selected)
            {
                flushText();
                removing = true;
            }
            else
            {
                if (removing)
                    reconcile();
                if (retainedString != item.sourceString)
                    flushText();
                retainedString = item.sourceString;
                if ((item.sourceCode.size() != 1 && item.sourceCode.size() != 2) ||
                    item.sourceString < 0)
                    fail("保持する生コードの文字列を確認できません。");
                auditPosition();
                retained += item.sourceCode;
                emittedCursor += float(item.width) * size / 1000;
                emittedCursor += float(item.wordSpacing);
                emittedCursor += spacing;
                emittedExact +=
                    item.width * operation.size / 1000 + operation.spacing + item.wordSpacing;
            }
            sourceCursor += float(item.width) * size / 1000;
            sourceCursor += float(item.wordSpacing);
            sourceCursor += spacing;
            sourceExact +=
                item.width * operation.size / 1000 + operation.spacing + item.wordSpacing;
            seenGlyph = true;
            if (bytes.size() + retained.size() > 64 * 1024 * 1024)
                fail("墨消し候補の内容が処理上限を超えています。");
        }
        auditPosition();
        flushText();
        return bytes + "] TJ\n";
    }
    QByteArray serializeText(const TextOperation& operation,
                             const QMap<QByteArray, QMap<CID, int>>& recoded,
                             const QSet<QByteArray>& rawFonts) const
    {
        const auto name = operation.resourceName;
        const bool rebuilt = recoded.contains(name);
        if (rebuilt && rawFonts.contains(name))
            return serializeRawText(operation);
        if (!operation.identity && !rebuilt &&
            std::any_of(operation.items.cbegin(), operation.items.cend(),
                        [](const TextItem& item) { return item.glyph && !item.selected; }))
            fail("独自CMapの部分墨消しは未対応です。");
        // A Form may inherit its caller's font without a local Tf. Rebuilding
        // its resource creates a different CID mapping; bind that local font
        // before emitting recoded text instead of inheriting the caller's map.
        QByteArray bytes = PDFDocumentWriter::getSerializedObject(PDFObject::createName(name)) +
                           " " + numeric(operation.size) + " Tf\n" + operation.prefix + "[",
                   retained;
        if (!rebuilt && operation.blankAnchorWidth &&
            std::all_of(operation.items.cbegin(), operation.items.cend(),
                        [](const TextItem& item) { return item.glyph; }))
        {
            // A leading numeric TJ changes the text object's origin and its
            // float rounding during rasterization. A known blank space keeps
            // the original origin. Only aggregate removed spans and surviving
            // positions enter the output, never individual removed widths.
            float sourceCursor = 0, emittedCursor = 0;
            bool started = false;
            auto move = [&](float target)
            {
                if (target != emittedCursor)
                {
                    const auto adjustment = redactionStandardTextAdjustment(
                        emittedCursor, target, float(operation.size), cancelled);
                    if (!adjustment)
                        fail("元の文字位置を必要な精度で再現できません。候補は出力していません。");
                    const auto encoded = numeric(double(*adjustment));
                    bool valid = false;
                    if (encoded.toFloat(&valid) != *adjustment || !valid)
                        fail("文字位置の数値を正確に保存できません。候補は出力していません。");
                    bytes += encoded + " ";
                }
                emittedCursor = target;
            };
            for (const auto& item : operation.items)
            {
                stop();
                const float advance = float(item.width) * float(operation.size) / 1000;
                if (!item.selected)
                {
                    if (item.cid > 255)
                        fail("標準書体の文字コードを確認できません。");
                    if (!started && sourceCursor != 0)
                    {
                        bytes += "<20> ";
                        emittedCursor =
                            float(*operation.blankAnchorWidth) * float(operation.size) / 1000;
                    }
                    move(sourceCursor);
                    const QByteArray code(1, char(item.cid));
                    bytes += "<" + code.toHex() + "> ";
                    emittedCursor += advance;
                    started = true;
                }
                sourceCursor += advance;
                if (bytes.size() > 64 * 1024 * 1024)
                    fail("墨消し候補の内容が処理上限を超えています。");
            }
            if (!started && sourceCursor != 0)
            {
                bytes += "<20> ";
                emittedCursor = float(*operation.blankAnchorWidth) * float(operation.size) / 1000;
            }
            move(sourceCursor);
            return bytes + "] TJ\n";
        }
        // Embedded CMaps use the declared advances in double precision. Rounding
        // a combined span to float can move the following retained glyphs.
        const bool embeddedCMap =
            operation.font->getFontType() == FontType::Type0 && !operation.identity;
        float removed = 0;
        double embeddedRemoved = 0;
        bool removing = false;
        auto flushText = [&]
        {
            if (!retained.isEmpty())
            {
                bytes += "<" + retained.toHex() + "> ";
                retained.clear();
            }
        };
        auto flushRemoved = [&]
        {
            if (removing)
            {
                const double advance = embeddedCMap ? embeddedRemoved : double(removed);
                bytes += numeric(-advance * 1000 / operation.size) + " ";
                removing = false;
                removed = 0;
                embeddedRemoved = 0;
            }
        };
        for (const auto& item : operation.items)
        {
            if (item.selected)
            {
                flushText();
                removing = true;
                if (embeddedCMap)
                    embeddedRemoved +=
                        item.width * operation.size / 1000 + operation.spacing + item.wordSpacing;
                else
                    removed += float(item.width) * float(operation.size) / 1000 +
                               float(operation.spacing + item.wordSpacing);
            }
            else if (!item.glyph)
            {
                if (removing)
                {
                    if (embeddedCMap)
                        embeddedRemoved -= item.advance * operation.size / 1000;
                    else
                        removed -= float(item.advance) * float(operation.size) / 1000;
                }
                else
                {
                    flushText();
                    bytes += numeric(item.advance) + " ";
                }
            }
            else
            {
                flushRemoved();
                const auto cid = rebuilt ? recoded[name].value(item.cid) : int(item.cid);
                if (rebuilt || operation.font->getFontType() == FontType::Type0)
                    retained.append(char(cid >> 8));
                else if (cid > 255)
                    fail("文字コードを安全に書き戻せません。");
                retained.append(char(cid));
                if (rebuilt && item.wordSpacing)
                {
                    flushText();
                    bytes += numeric(-item.wordSpacing * 1000 / operation.size) + " ";
                }
            }
        }
        flushText();
        flushRemoved();
        return bytes + "] TJ\n";
    }
    bool coversAdvanceSegment(QPointF first, QPointF last) const
    {
        const auto delta = last - first;
        if (!std::isfinite(first.x()) || !std::isfinite(first.y()) || !std::isfinite(last.x()) ||
            !std::isfinite(last.y()) || !std::isfinite(delta.x()) || !std::isfinite(delta.y()))
            fail("空白文字の範囲が不正です。墨消ししていません。");
        QVector<QPair<double, double>> intervals;
        for (const auto& rectangle : rectangles)
        {
            stop();
            double from = 0, to = 1;
            auto clip = [&](double origin, double direction, double low, double high)
            {
                if (direction == 0)
                    return low <= origin && origin <= high;
                double begin = (low - origin) / direction, end = (high - origin) / direction;
                if (!std::isfinite(begin) || !std::isfinite(end))
                    fail("空白文字の範囲を安全に計算できません。墨消ししていません。");
                if (begin > end)
                    std::swap(begin, end);
                from = qMax(from, begin);
                to = qMin(to, end);
                return from <= to;
            };
            if (clip(first.x(), delta.x(), rectangle.left(), rectangle.right()) &&
                clip(first.y(), delta.y(), rectangle.top(), rectangle.bottom()))
                intervals << qMakePair(from, to);
        }
        std::sort(intervals.begin(), intervals.end());
        double covered = 0;
        for (const auto& interval : intervals)
        {
            if (interval.first > covered)
                return false;
            covered = qMax(covered, interval.second);
            if (covered >= 1)
                return true;
        }
        return false;
    }
    bool covers(const QRectF& bounds) const
    {
        // QPainterPath::contains rejects coincident boundaries. A closed
        // rectangle fully covers its own boundary without adding any margin.
        return std::any_of(rectangles.begin(), rectangles.end(),
                           [&](const QRectF& rectangle) { return rectangle.contains(bounds); }) ||
               regions.contains(bounds);
    }
    void prepareForm(const PDFDictionary* dictionary, const PDFObject& original)
    {
        if (!original.isReference() || dictionary->hasKey("OC") || dictionary->hasKey("Ref") ||
            dictionary->hasKey("F"))
            fail("このForm XObjectの共有情報や外部参照を安全に確認できません。");
        const auto formType = getDocument()->getObject(dictionary->get("FormType"));
        if (!formType.isNull() && (!formType.isInt() || formType.getInteger() != 1))
            fail("このForm XObjectの形式には対応していません。");
        const auto box = getDocument()->getObject(dictionary->get("BBox"));
        if (!box.isArray() || box.getArray()->getCount() != 4)
            fail("Form XObjectの描画境界を確認できません。");
        const auto array = box.getArray();
        const QRectF bounds(QPointF(number(array->getItem(0)), number(array->getItem(1))),
                            QPointF(number(array->getItem(2)), number(array->getItem(3))));
        if (bounds.width() <= 0 || bounds.height() <= 0)
            fail("Form XObjectの描画境界が不正です。");
        QTransform transform;
        const auto matrix = getDocument()->getObject(dictionary->get("Matrix"));
        if (!matrix.isNull())
        {
            if (!matrix.isArray() || matrix.getArray()->getCount() != 6)
                fail("Form XObjectの座標変換を確認できません。");
            const auto values = matrix.getArray();
            transform = {number(values->getItem(0)), number(values->getItem(1)),
                         number(values->getItem(2)), number(values->getItem(3)),
                         number(values->getItem(4)), number(values->getItem(5))};
        }
        transform = transform * getGraphicState()->getCurrentTransformationMatrix();
        if (!transform.isInvertible())
            fail("Form XObjectの座標変換が不正です。");
        const auto transformed = transform.mapRect(bounds);
        const auto painted = transformed.intersected(pageClip);
        if (!std::isfinite(painted.x()) || !std::isfinite(painted.y()) ||
            !std::isfinite(painted.width()) || !std::isfinite(painted.height()))
            fail("Form XObjectの描画境界が処理上限を超えています。");
        const bool intersects = regions.intersects(painted);
        if (!intersects && shared && shared->forbiddenForm(original))
            fail("削除対象の元Form内容が範囲外にも共有されています。候補は出力していません。");
        const bool recode = shared && shared->affectedResources(dictionary->get("Resources"));
        if (intersects || recode)
        {
            if (!intersects || !covers(painted))
            {
                if (formDepth >= 8 || dictionary->hasKey("Group") ||
                    dictionary->hasKey("StructParent") || dictionary->hasKey("StructParents") ||
                    !dictionary->hasKey("Resources"))
                    fail("このForm XObjectの部分墨消しは安全に処理できません。");
                const bool aligned = (transform.m12() == 0 && transform.m21() == 0) ||
                                     (transform.m11() == 0 && transform.m22() == 0);
                if (!aligned)
                    fail("傾いたForm XObjectの部分墨消しは未対応です。");
                const auto inverse = transform.inverted();
                QVector<QRectF> localRectangles;
                QPainterPath localRegions;
                for (const auto& rectangle : rectangles)
                {
                    const auto local = inverse.mapRect(rectangle).intersected(bounds);
                    if (!local.isEmpty())
                    {
                        localRectangles << local;
                        localRegions.addRect(local);
                    }
                }
                if (intersects)
                {
                    if (shared)
                        shared->validateFormSelection(original, localRegions);
                    rememberFormRegion(original.getReference(), localRegions);
                }
                ContentProcessor child(getPage(), getDocument(), getFontCache(), getCMS(),
                                       localRegions, builder, cancelled, shared, cvtUsage);
                child.formDepth = formDepth + 1;
                child.activeFont = activeFont;
                child.pageClip = inverse.mapRect(pageClip).intersected(bounds);
                child.rectangles = localRectangles;
                child.initializeProcessor();
                auto state = *getGraphicState();
                state.setCurrentTransformationMatrix(QTransform());
                child.setGraphicsState(state);
                const auto stream = getDocument()->getObject(original);
                child.processForm(QTransform(), bounds, dictionary->get("Resources"), {},
                                  getDocument()->getDecodedStream(stream.getStream()), -1);
                const auto content = child.finish();
                PDFDictionary resources;
                detail::set(resources, "Font", detail::dictObject(content.fonts));
                detail::set(resources, "XObject", detail::dictObject(content.xobjects));
                detail::set(resources, "ColorSpace", detail::dictObject(content.colorSpaces));
                detail::set(resources, "ExtGState", detail::dictObject(content.graphicsStates));
                PDFDictionary form;
                detail::set(form, "Type", PDFObject::createName("XObject"));
                detail::set(form, "Subtype", PDFObject::createName("Form"));
                detail::set(form, "BBox", PDFObject(dictionary->get("BBox")));
                if (dictionary->hasKey("Matrix"))
                    detail::set(form, "Matrix", PDFObject(dictionary->get("Matrix")));
                detail::set(form, "Resources", detail::dictObject(resources));
                replacementImage = "TatsujinRedactForm" + QByteArray::number(++formSuffix);
                while (result.xobjects.hasKey(replacementImage) ||
                       getXObjectDictionary()->hasKey(replacementImage))
                    replacementImage = "TatsujinRedactForm" + QByteArray::number(++formSuffix);
                result.xobjects.setEntry(PDFInplaceOrMemoryString(replacementImage),
                                         PDFObject::createReference(builder.addObject(
                                             detail::streamObject(form, content.bytes))));
                result.removedTextSegments += content.removedTextSegments;
                result.modifiedImages += content.modifiedImages;
                result.removedContentGroups += content.removedContentGroups;
                result.modifiedImageSources.insert(content.modifiedImageSources.begin(),
                                                   content.modifiedImageSources.end());
                result.removedResourceDependencies.insert(
                    content.removedResourceDependencies.begin(),
                    content.removedResourceDependencies.end());
                result.removedFormSources.insert(content.removedFormSources.begin(),
                                                 content.removedFormSources.end());
                for (auto it = content.redactedFormRegions.cbegin();
                     it != content.redactedFormRegions.cend(); ++it)
                    rememberFormRegion(it.key(), it.value());
                // The source form's bytes may expose the removed text even if
                // reached through a private alias or a duplicate stream.
                result.removedResourceDependencies.insert(original.getReference());
                if (intersects)
                    result.removedFormSources.insert(original.getReference());
                suppressForm = true;
                return;
            }
            rememberRemovedDependencies(original);
            result.removedFormSources.insert(original.getReference());
            QPainterPath whole;
            whole.addRect(bounds);
            if (shared)
                shared->validateFormSelection(original, whole);
            rememberFormRegion(original.getReference(), whole);
            removeForm = true;
        }
        // The form BBox and page CropBox both clip painting. Whole visible
        // groups are removed with their hidden clipped content and dependencies;
        // disjoint groups remain verbatim. A partly selected group is refused.
        suppressForm = true;
    }
    bool requiresFontCleanup() const
    {
        const auto declaration =
            getDocument()->getDictionaryFromObject(getFontDictionary()->get(activeFont.name));
        if (!declaration)
            fail("文字の書体情報を確認できません。");
        auto subset = [&](const PDFObject& value)
        {
            const auto object = getDocument()->getObject(value);
            static const QRegularExpression tag("^[A-Z]{6}\\+");
            return object.isName() && tag.match(QString::fromLatin1(object.getString())).hasMatch();
        };
        auto check = [&](const PDFDictionary* dictionary)
        {
            if (dictionary->hasKey("ToUnicode") || subset(dictionary->get("BaseFont")) ||
                requiresRedactionFontSchemaCleanup(*getDocument(), *dictionary))
                return true;
            if (const auto descriptor =
                    getDocument()->getDictionaryFromObject(dictionary->get("FontDescriptor")))
                return subset(descriptor->get("FontName")) || descriptor->hasKey("FontFile") ||
                       descriptor->hasKey("FontFile2") || descriptor->hasKey("FontFile3");
            return false;
        };
        if (check(declaration))
            return true;
        const auto descendants = getDocument()->getObject(declaration->get("DescendantFonts"));
        if (descendants.isArray())
            for (const auto& child : *descendants.getArray())
                if (const auto dictionary = getDocument()->getDictionaryFromObject(child))
                    if (check(dictionary))
                        return true;
        return false;
    }
    void rememberFormRegion(PDFObjectReference reference, const QPainterPath& region)
    {
        if (result.redactedFormRegions.contains(reference) &&
            result.redactedFormRegions[reference] != region)
            fail("同じFormへの異なる範囲指定は安全に処理できません。候補は出力していません。");
        result.redactedFormRegions[reference] = region;
    }
    void rememberRemovedDependencies(const PDFObject& font)
    {
        std::vector<PDFObject> pending{font};
        std::set<PDFObjectReference> visited;
        size_t examined = 0;
        while (!pending.empty())
        {
            stop();
            if (++examined > 4096 || pending.size() > 4096)
                fail("サブセット書体の参照が処理上限を超えています。");
            auto object = std::move(pending.back());
            pending.pop_back();
            if (object.isReference())
            {
                const auto reference = object.getReference();
                result.removedResourceDependencies.insert(reference);
                if (!visited.insert(reference).second)
                    continue;
                object = getDocument()->getObjectByReference(reference);
                if (object.isStream() &&
                    getDocument()->getObject(object.getStream()->getDictionary()->get("Subtype")) ==
                        PDFObject::createName("Form"))
                    result.removedFormSources.insert(reference);
            }
            const auto dictionary = object.isDictionary() ? object.getDictionary()
                                    : object.isStream()   ? object.getStream()->getDictionary()
                                                          : nullptr;
            if (dictionary)
            {
                if (dictionary->getCount() + pending.size() > 4096)
                    fail("サブセット書体の参照が処理上限を超えています。");
                for (size_t i = 0; i < dictionary->getCount(); ++i)
                    pending.push_back(dictionary->getValue(i));
            }
            else if (object.isArray())
            {
                if (object.getArray()->getCount() + pending.size() > 4096)
                    fail("サブセット書体の参照が処理上限を超えています。");
                for (const auto& item : *object.getArray())
                    pending.push_back(item);
            }
        }
    }
    double number(const PDFObject& value) const
    {
        const auto object = getDocument()->getObject(value);
        if (!object.isInt() && !object.isReal())
            fail("書体の文字幅が不正です。");
        const auto width = object.isInt() ? double(object.getInteger()) : object.getReal();
        if (!std::isfinite(width) || std::abs(width) > 10000000)
            fail("書体の文字幅が処理上限を超えています。");
        return width;
    }
    double glyphWidth(CID cid)
    {
        const auto declaration =
            getDocument()->getDictionaryFromObject(getFontDictionary()->get(activeFont.name));
        if (!declaration)
            fail("書体の文字幅を確認できません。");
        const auto descendants = getDocument()->getObject(declaration->get("DescendantFonts"));
        if (descendants.isArray() && descendants.getArray()->getCount() == 1)
        {
            const auto descendant =
                getDocument()->getDictionaryFromObject(descendants.getArray()->getItem(0));
            if (!descendant)
                fail("複合書体の文字幅が不正です。");
            const auto widths = getDocument()->getObject(descendant->get("W"));
            if (!widths.isNull() && !widths.isArray())
                fail("複合書体の文字幅一覧が不正です。");
            if (widths.isArray())
            {
                const auto array = widths.getArray();
                if (array->getCount() > 100000)
                    fail("文字幅一覧が処理上限を超えています。");
                for (size_t i = 0; i < array->getCount();)
                {
                    stop();
                    const auto start = number(array->getItem(i++));
                    if (i >= array->getCount())
                        fail("文字幅一覧が途中で終わっています。");
                    const auto entry = getDocument()->getObject(array->getItem(i++));
                    if (entry.isArray())
                    {
                        if (double(cid) >= start &&
                            double(cid) < start + entry.getArray()->getCount())
                            return number(entry.getArray()->getItem(size_t(double(cid) - start)));
                    }
                    else
                    {
                        const auto end = number(entry);
                        if (i >= array->getCount() || end < start)
                            fail("文字幅の範囲が不正です。");
                        const auto width = number(array->getItem(i++));
                        if (double(cid) >= start && double(cid) <= end)
                            return width;
                    }
                }
            }
            return descendant->hasKey("DW") ? number(descendant->get("DW")) : 1000;
        }
        const auto widths = getDocument()->getObject(declaration->get("Widths"));
        if (widths.isArray())
        {
            const auto first = number(declaration->get("FirstChar"));
            if (double(cid) < first || double(cid) - first >= widths.getArray()->getCount())
                fail("文字幅一覧に必要な文字がありません。");
            return number(widths.getArray()->getItem(size_t(double(cid) - first)));
        }
        const auto base = getDocument()->getObject(declaration->get("BaseFont"));
        if (!base.isName())
            fail("明示された文字幅がない非標準書体の墨消しは未評価です。");
        const auto declaredEncoding = getDocument()->getObject(declaration->get("Encoding"));
        QByteArray encoding = base.getString() == "Symbol"         ? "SymbolEncoding"
                              : base.getString() == "ZapfDingbats" ? "ZapfDingbatsEncoding"
                                                                   : "StandardEncoding";
        if (!declaredEncoding.isNull())
        {
            if (!declaredEncoding.isName())
                fail("標準書体の独自エンコーディングの墨消しは未評価です。");
            encoding = declaredEncoding.getString();
        }
        const auto width = detail::standardFontGlyphWidth(base.getString(), encoding, cid);
        if (width < 0)
            fail("標準書体の文字幅を確認できません。");
        return width;
    }
    void stop() const
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
    }
    QPainterPath regions;
    PDFDocumentBuilder& builder;
    std::function<bool()> cancelled;
    const RedactionResourceUsage* shared;
    RedactionFontCvtUsage* cvtUsage;
    RedactedPageContent result;
    QByteArray command, serialized, imageName, replacementImage;
    bool suppressForm = false, removeForm = false;
    bool removePath = false;
    bool pendingClip = false, clipValidated = false, finishClip = false, rectanglePath = false;
    qsizetype clipOperation = -1;
    QVector<qsizetype> pathOperations;
    QRectF pageClip;
    std::set<QByteArray> selectedFontsNeedingCleanup;
    // PDFFontCache keys by object reference; getFontId() may name another alias.
    // Resource names belong to the current graphics state and resource scope.
    FontResource activeFont;
    QVector<FontResource> fontStack;
    QVector<Operation> operations;
    QMap<QByteArray, std::shared_ptr<RedactionCMap>> customMaps;
    std::shared_ptr<TextOperation> textOperation;
    QString analysisError;
    std::exception_ptr deferredError;
    int graphicsDepth = 0, selectedCharacters = 0;
    int formDepth = 0, formSuffix = 0;
};
} // namespace
QPainterPath checkedRedactionRegions(const PDFDocument& document, int page,
                                     const QVector<QRectF>& rectangles)
{
    if (!document.getCatalog() || !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
    if (page < 0 || page >= int(document.getCatalog()->getPageCount()) || rectangles.isEmpty() ||
        rectangles.size() > 1000)
        fail("墨消しするページと範囲を確認してください。");
    QPainterPath regions;
    for (const auto& rectangle : rectangles)
    {
        if (!std::isfinite(rectangle.x()) || !std::isfinite(rectangle.y()) ||
            !std::isfinite(rectangle.width()) || !std::isfinite(rectangle.height()) ||
            rectangle.isEmpty() ||
            !document.getCatalog()->getPage(page)->getMediaBox().contains(rectangle))
            fail("墨消しの範囲がページ外または不正です。");
        QPainterPath path;
        path.addRect(rectangle);
        regions = regions.united(path);
    }
    return regions;
}
RedactedPageContent
redactPageContent(const PDFDocument& document, int page, const QVector<QRectF>& rectangles,
                  PDFDocumentBuilder& builder, const std::function<bool()>& cancelled,
                  const RedactionResourceUsage* shared, RedactionFontCvtUsage* cvtUsage)
{
    auto regions = rectangles.isEmpty() && shared
                       ? QPainterPath()
                       : checkedRedactionRegions(document, page, rectangles);
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    const auto pdfPage = document.getCatalog()->getPage(page);
    auto inspect = [&](const PDFObject& value)
    {
        const auto object = document.getObject(value);
        if (!object.isStream())
            fail("ページ内容が不正です。");
        const auto bytes = document.getDecodedStream(object.getStream());
        if (bytes.size() > 64 * 1024 * 1024)
            fail("ページ内容が墨消し処理の上限を超えています。");
        PDFLexicalAnalyzer analyzer(bytes.constBegin(), bytes.constEnd());
        while (!analyzer.isAtEnd())
        {
            if (cancelled && cancelled())
                fail("墨消しを中止しました。");
            const auto token = analyzer.fetch();
            if (token.type == PDFLexicalAnalyzer::TokenType::Command &&
                token.data.toByteArray() == "BI")
                fail("インライン画像の安全な墨消しにはまだ対応していません。");
        }
    };
    const auto contents = document.getObject(pdfPage->getContents());
    if (contents.isArray())
        for (const auto& object : *contents.getArray())
            inspect(object);
    else if (!contents.isNull())
        inspect(contents);
    auto snapshot = document;
    PDFFontCache fonts{128, 128};
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    ContentProcessor processor(pdfPage, &document, &fonts, cms.data(), regions, builder, cancelled,
                               shared, cvtUsage);
    processor.rectangles = rectangles;
    for (const auto& error : processor.processContents())
        if (error.type != RenderErrorType::Information)
            fail("墨消し前の内容解析に失敗しました: " + error.message);
    return processor.finish();
}
RedactedPageContent
reencodeRedactionAppearance(const PDFDocument& document, int page, const PDFObject& appearance,
                            PDFDocumentBuilder& builder, const RedactionResourceUsage& shared,
                            const std::function<bool()>& cancelled, RedactionFontCvtUsage* cvtUsage)
{
    const auto object = document.getObject(appearance);
    if (!appearance.isReference() || !object.isStream() || shared.forbiddenForm(appearance))
        fail("注釈外観の共有参照を安全に確認できません。");
    const auto dictionary = object.getStream()->getDictionary();
    for (const auto key : {"Group", "OC", "Ref", "F", "StructParent", "StructParents"})
        if (dictionary->hasKey(key))
            fail("この注釈外観の安全な字体再構築は未評価です。");
    if (document.getObject(dictionary->get("Subtype")) != PDFObject::createName("Form") ||
        !dictionary->hasKey("Resources"))
        fail("注釈外観の形式または資源を確認できません。");
    PDFDocumentDataLoaderDecorator loader(&document);
    const auto bounds = loader.readRectangle(dictionary->get("BBox"), {});
    if (bounds.isEmpty() || !std::isfinite(bounds.x()) || !std::isfinite(bounds.y()) ||
        !std::isfinite(bounds.width()) || !std::isfinite(bounds.height()))
        fail("注釈外観の描画範囲が不正です。");
    const auto bytes = document.getDecodedStream(object.getStream());
    if (bytes.size() > 64 * 1024 * 1024)
        fail("注釈外観が処理上限を超えています。");
    auto snapshot = document;
    PDFFontCache fonts{128, 128};
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    ContentProcessor processor(document.getCatalog()->getPage(page), &document, &fonts, cms.data(),
                               {}, builder, cancelled, &shared, cvtUsage);
    processor.initializeProcessor();
    processor.processForm(QTransform(), bounds, dictionary->get("Resources"), {}, bytes, -1);
    return processor.finish();
}
} // namespace tatsu
