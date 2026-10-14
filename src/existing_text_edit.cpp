#include "existing_text_edit.h"
#include "body_text_font.h"
#include "body_text_wrap.h"
#include "content_callback_guard.h"
#include "content_invocation_edit.h"
#include "form_content_invocations.h"
#include "pdf_font_metrics.h"
#include "pdf_objects.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include "pdfparser.h"
#include "pdfrenderer.h"
#include "pdftextlayoutgenerator.h"
#include <cmath>

namespace tatsu
{
namespace
{
using namespace detail;
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("本文の編集を中止しました。");
}
struct Span
{
    qsizetype begin = 0, end = 0;
};
struct TextPart
{
    ExistingTextBlock value;
    PDFFontPointer font;
    QByteArray originalCodes;
    QByteArray fontName;
    QTransform matrix, ctm;
    QPointF origin, end;
    double fontSize = 0;
    QVector<QRectF> glyphs;
};
struct Block
{
    ExistingTextBlock value;
    Span matrixSpan;
    QVector<Span> showSpans;
    QVector<TextPart> parts;
    QString partsRestriction;
    QString flowRestriction;
    QString linesRestriction;
    int physicalLine = 0, lineShows = 0;
    double flowLeading = 0;
    int matrices = 0, shows = 0, advances = 0;
    bool stateChanged = false;
    int pendingMatrices = 0;
    QByteArray advanceCommand;
    double advanceDy = 0;
    QTransform matrix, ctm;
    PDFFontPointer font;
    double fontSize = 0;
    double endingLeading = 0;
    QByteArray originalCodes;
    QByteArray fontName;
    int owner = 0;
};
struct Inspection
{
    QByteArray bytes;
    QVector<Block> blocks;
    QVector<ContentInvocationOwner> owners;
};
QByteArray contents(const PDFDocument& document, PDFObject value)
{
    value = document.getObject(value);
    QByteArray bytes;
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
            fail("本文のページ内容を確認できません。");
        bytes += document.getDecodedStream(stream.getStream()) + '\n';
        if (bytes.size() > 64 * 1024 * 1024)
            fail("本文を編集するページ内容が上限を超えています。");
    }
    return bytes;
}
QVector<Block> spans(const QByteArray& bytes, const std::function<bool()>& cancelled)
{
    PDFLexicalAnalyzer lexer(bytes.constBegin(), bytes.constEnd());
    QVector<Block> blocks;
    bool inside = false;
    qsizetype operand = -1;
    int arrays = 0;
    QVector<double> numbers;
    bool numericOperands = true;
    QByteArray pendingCodes;
    while (!lexer.isAtEnd())
    {
        stop(cancelled);
        lexer.skipWhitespaceAndComments();
        const auto begin = lexer.pos();
        const auto token = lexer.fetch();
        using Type = PDFLexicalAnalyzer::TokenType;
        if (token.type == Type::EndOfFile)
            break;
        if (token.type == Type::ArrayStart)
            ++arrays;
        if (token.type == Type::ArrayEnd && --arrays < 0)
            fail("本文の文字配列が不正です。");
        if (token.type != Type::Command)
        {
            if (token.type == Type::Integer || token.type == Type::Real)
                numbers << token.data.toDouble();
            else
                numericOperands = false;
            if (inside && token.type == Type::String)
            {
                blocks.last().originalCodes += token.data.toByteArray();
                pendingCodes += token.data.toByteArray();
            }
            if (operand < 0)
                operand = begin;
            continue;
        }
        if (arrays)
            fail("本文の文字配列に未対応の命令があります。");
        const auto command = token.data.toByteArray();
        if (command == "BI" || command == "W" || command == "W*")
            fail("インライン画像やクリッピングがあるページの本文編集は未対応です。");
        if (command == "BT")
        {
            if (inside || blocks.size() >= 1000)
                fail("本文の文字ブロックを確認できません。");
            inside = true;
            blocks << Block{};
            blocks.last().value.occurrence = blocks.size() - 1;
        }
        else if (command == "ET")
        {
            if (!inside)
                fail("本文の文字ブロックの終端が不正です。");
            inside = false;
        }
        else if (inside)
        {
            auto& block = blocks.last();
            const Span span{operand < 0 ? begin : operand, lexer.pos()};
            if (command == "Tm")
            {
                block.matrixSpan = span;
                ++block.matrices;
                ++block.pendingMatrices;
            }
            else if (command == "Tj" || command == "TJ")
            {
                if (block.shows && (block.advances != block.shows || block.stateChanged))
                    block.value.restriction = "一定の字体と行送りを持つ複数行を選んでください。";
                block.showSpans << span;
                TextPart part;
                part.originalCodes = pendingCodes;
                part.value.physicalLine = block.physicalLine;
                block.parts << part;
                if (block.pendingMatrices != 1)
                    block.partsRestriction = "各文字部分の前に独立した位置指定が必要です。";
                block.pendingMatrices = 0;
                ++block.shows;
                ++block.lineShows;
                block.stateChanged = false;
            }
            else if (command == "T*" || command == "Td" || command == "TD")
            {
                block.flowRestriction =
                    "後続を送る編集では相対移動・改行のない1行を選んでください。";
                block.partsRestriction = "流し込みや相対位置の文字部分編集は未対応です。";
                if (!block.shows || block.advances != block.shows - 1)
                    block.value.restriction = "各文字描画の間に1回改行する本文を選んでください。";
                const bool relative = command != "T*";
                const bool valid =
                    numericOperands && (relative ? numbers.size() == 2 && numbers[0] == 0 &&
                                                       std::isfinite(numbers[1]) && numbers[1] < 0
                                                 : numbers.isEmpty());
                if (!block.lineShows || !valid ||
                    (!block.advanceCommand.isEmpty() &&
                     (block.advanceCommand != command ||
                      (relative && block.advanceDy != numbers.value(1)))))
                    block.linesRestriction =
                        "空行・横移動のない一定の改行命令と行送りを選んでください。";
                ++block.physicalLine;
                block.lineShows = 0;
                if (!valid || (!block.advanceCommand.isEmpty() &&
                               (block.advanceCommand != command ||
                                (relative && block.advanceDy != numbers.value(1)))))
                    block.value.restriction =
                        "横移動のない一定の改行命令と行送りを持つ本文を選んでください。";
                if (block.advanceCommand.isEmpty())
                {
                    block.advanceCommand = command;
                    block.advanceDy = numbers.value(1);
                }
                ++block.advances;
            }
            else if (command != "Tf" && command != "Tc" && command != "Tw" && command != "Tz" &&
                     command != "TL" && command != "Tr" && command != "Ts")
            {
                block.value.restriction =
                    "複数行や非対応の本文命令を含むブロックは編集できません。";
                block.partsRestriction = "非対応の命令がある文字部分は編集できません。";
                block.flowRestriction = block.partsRestriction;
                block.linesRestriction = block.partsRestriction;
            }
            else if (block.shows)
                block.stateChanged = true;
        }
        operand = -1;
        numbers.clear();
        numericOperands = true;
        pendingCodes.clear();
    }
    if (inside || arrays)
        fail("本文の文字ブロックが閉じられていません。");
    for (auto& block : blocks)
    {
        if (block.matrices != 1 || block.shows < 1 || block.shows > 64 || block.physicalLine < 1 ||
            block.physicalLine >= 64 || !block.lineShows ||
            block.matrixSpan.end > block.showSpans.first().begin)
            block.linesRestriction =
                "先頭の位置指定1個と2〜64行、64描画以内の本文を選んでください。";
        if (block.matrices != 1 || block.shows < 1 || block.shows > 64 ||
            block.matrixSpan.end > block.showSpans.first().begin)
            block.flowRestriction =
                "後続を送る編集では先頭に1個の位置指定がある64描画以内の1行を選んでください。";
        if (block.shows < 1 || block.shows > 64 || block.pendingMatrices)
            block.partsRestriction = "各部分に位置指定がある64描画以内の本文を選んでください。";
        if (block.matrices != 1 || block.shows < 1 || block.shows > 64 ||
            block.advances != block.shows - 1 ||
            block.matrixSpan.end > block.showSpans.first().begin)
            block.value.restriction =
                "1個の位置行列と一定の行送りを持つ64行以内のブロックを選んでください。";
    }
    return blocks;
}
class Collector : public PDFTextLayoutGenerator
{
public:
    using PDFTextLayoutGenerator::PDFTextLayoutGenerator;
    QVector<Block>* blocks = nullptr;
    std::function<bool()> cancelled;
    FormContentInvocations* invocations = nullptr;
    QHash<const PDFFont*, PDFObject> fontBindings;
    ContentCallbackGuard boundary;
    struct Frame
    {
        QVector<int> indices;
        int cursor = -1, line = 0;
        bool inside = false, firstCharacter = false;
    };
    QVector<Frame> frames;
    void initialize()
    {
        Frame root;
        for (int i = 0; i < blocks->size(); ++i)
            root.indices << i;
        frames << root;
    }
    bool finished() const
    {
        boundary.check();
        for (const auto& frame : frames)
            if (frame.inside || frame.cursor + 1 != frame.indices.size())
                return false;
        return !invocations || invocations->finished();
    }

protected:
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        return (kind == ContentKind::Forms && !invocations) || kind == ContentKind::Images ||
               PDFTextLayoutGenerator::isContentKindSuppressed(kind);
    }
    void performInterceptInstruction(Operator, ProcessOrder order,
                                     const QByteArray& command) override
    {
        boundary.invoke(order == ProcessOrder::AfterOperation,
                        [&] { processInstruction(order, command); });
    }
    void processInstruction(ProcessOrder order, const QByteArray& command)
    {
        stop(cancelled);
        if (command == "Tf" && order == ProcessOrder::AfterOperation)
        {
            const auto font = getGraphicState()->getTextFont();
            if (font && getFontDictionary() && getOperands().size() == 2)
                fontBindings[font.data()] =
                    getFontDictionary()->get(getOperands()[0].data.toByteArray());
        }
        if (invocations && command == "Do")
        {
            if (order == ProcessOrder::AfterOperation)
            {
                invocations->endDrawing();
                return;
            }
            if (getOperands().size() != 1)
                fail("本文グループの描画参照を確認できません。");
            const auto call = invocations->beginDrawing(
                getOperands()[0].data.toByteArray(), getXObjectDictionary(),
                getGraphicState()->getCurrentTransformationMatrix());
            if (call.subtype == "Form")
            {
                const int owner = invocations->currentOwner();
                if (owner != frames.size())
                    fail("本文グループの対応を確認できません。");
                Frame frame;
                for (auto block : spans(invocations->owners[owner].bytes, cancelled))
                {
                    if (blocks->size() >= 1000)
                        fail("本文ブロックの総数が1000を超えています。");
                    block.owner = owner;
                    block.value.depth = invocations->depth();
                    block.value.occurrence = blocks->size();
                    frame.indices << blocks->size();
                    blocks->append(block);
                }
                frames << frame;
            }
            return;
        }
        if (order != ProcessOrder::BeforeOperation)
        {
            if (command == "Tj" || command == "TJ")
            {
                const auto& frame = frames[invocations ? invocations->currentOwner() : 0];
                if (frame.inside && frame.line > 0)
                {
                    const auto state = getGraphicState();
                    auto& part = (*blocks)[frame.indices[frame.cursor]].parts[frame.line - 1];
                    part.end = (state->getTextMatrix() * state->getCurrentTransformationMatrix() *
                                getPagePointToDevicePointMatrix())
                                   .map(QPointF());
                }
            }
            return;
        }
        auto& frame = frames[invocations ? invocations->currentOwner() : 0];
        if (command == "BT")
        {
            if (++frame.cursor >= frame.indices.size())
                fail("本文命令の対応を確認できません。");
            frame.inside = true;
            frame.line = 0;
        }
        else if (command == "ET")
            frame.inside = false;
        else if (frame.inside && command == "T*")
        {
            auto& block = (*blocks)[frame.indices[frame.cursor]];
            const double leading = getGraphicState()->getTextLeading();
            if (!std::isfinite(leading) || leading <= 0 ||
                (block.flowLeading != 0 && block.flowLeading != leading))
                block.linesRestriction = "正の一定行送りを持つ複数行を選んでください。";
            block.flowLeading = leading;
        }
        else if (frame.inside && (command == "Tj" || command == "TJ"))
        {
            auto& block = (*blocks)[frame.indices[frame.cursor]];
            const auto state = getGraphicState();
            if (frame.line >= block.parts.size())
                fail("本文の文字部分の対応を確認できません。");
            auto& part = block.parts[frame.line];
            part.matrix = state->getTextMatrix();
            part.ctm = state->getCurrentTransformationMatrix();
            part.fontSize = state->getTextFontSize();
            part.origin =
                (part.matrix * part.ctm * getPagePointToDevicePointMatrix()).map(QPointF());
            part.font = state->getTextFont();
            if (part.font)
            {
                part.value.font = QString::fromLatin1(part.font->getFontDescriptor()->fontName);
                part.fontName = resourceFontName(part.font);
            }
            if (!std::isfinite(state->getTextFontSize()) || state->getTextFontSize() <= 0 ||
                state->getTextCharacterSpacing() != 0 || state->getTextWordSpacing() != 0 ||
                state->getTextHorizontalScaling() != 100 || state->getTextRise() != 0 ||
                !state->getTextMatrix().isInvertible())
                part.value.restriction = "通常の横書きと文字間隔を持つ文字部分を選んでください。";
            block.endingLeading = state->getTextLeading();
            if (block.shows > 1 && block.advanceCommand == "T*" &&
                (!std::isfinite(state->getTextLeading()) || state->getTextLeading() <= 0))
                block.value.restriction = "正の一定行送りを持つ複数行を選んでください。";
            frame.firstCharacter = true;
            if (!frame.line++)
            {
                block.font = state->getTextFont();
                block.fontSize = state->getTextFontSize();
                block.matrix = state->getTextMatrix();
                block.ctm = state->getCurrentTransformationMatrix();
                if (block.font)
                {
                    block.value.font =
                        QString::fromLatin1(block.font->getFontDescriptor()->fontName);
                    block.fontName = resourceFontName(block.font);
                    if (block.fontName.isEmpty())
                        block.value.restriction = "本文の字体資源を確認できません。";
                }
            }
            if (state->getTextRenderingMode() != TextRenderingMode::Fill ||
                state->getAlphaFilling() != 1 || isContentSuppressed())
            {
                block.value.restriction =
                    "不可視OCR・文字クリップ・透過文字の本文編集は未対応です。";
                part.value.restriction = block.value.restriction;
            }
        }
    }
    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        auto& frame = frames[invocations ? invocations->currentOwner() : 0];
        if (!frame.inside || isContentSuppressed())
            return;
        auto& block = (*blocks)[frame.indices[frame.cursor]];
        auto& part = block.parts[frame.line - 1];
        part.value.text += info.character;
        if (frame.firstCharacter && frame.line > 1 && !block.value.text.isEmpty())
            block.value.text += '\n';
        frame.firstCharacter = false;
        block.value.text += info.character;
        if (block.value.text.size() > 4096)
            fail("本文ブロックの文字数が上限を超えています。");
        if (info.isVerticalWritingSystem)
        {
            block.value.restriction = "縦書き本文の編集は未対応です。";
            part.value.restriction = block.value.restriction;
        }
        const auto box =
            getPagePointToDevicePointMatrix().map(info.matrix.map(info.outline)).boundingRect();
        if (box.isValid())
        {
            part.glyphs << box;
            block.value.physical = block.value.physical.united(box);
            part.value.physical = part.value.physical.united(box);
            if (invocations &&
                !contentBoundsContain(invocations->owners[block.owner].clips, QPolygonF(box)))
            {
                block.value.restriction = "本文がグループの表示範囲からはみ出しています。";
                part.value.restriction = block.value.restriction;
            }
        }
    }

private:
    QByteArray resourceFontName(const PDFFontPointer& font) const
    {
        const auto resources = getFontDictionary();
        if (!resources)
            return {};
        const auto preferred = font->getFontId();
        const auto binding = fontBindings.constFind(font.data());
        if (binding != fontBindings.cend())
        {
            if (resources->hasKey(preferred) && resources->get(preferred) == binding.value())
                return preferred;
            for (size_t i = 0; i < resources->getCount(); ++i)
                if (resources->getValue(i) == binding.value())
                    return resources->getKey(i).getString();
        }
        if (resources->getCount() > 128)
            return {};
        for (size_t i = 0; i < resources->getCount(); ++i)
        {
            if (!resources->getValue(i).isReference())
                continue;
            const auto name = resources->getKey(i).getString();
            try
            {
                if (getFontCache()->getFont(resources->getValue(i), name) == font)
                    return name;
            }
            catch (const PDFException&)
            {
            }
        }
        return {};
    }
};

Inspection inspect(const PDFDocument& document, int page, const std::function<bool()>& cancelled,
                   bool includeForms,
                   ExistingTextPartLayout layout = ExistingTextPartLayout::Positioned)
{
    stop(cancelled);
    if (page < 0 || page >= int(document.getCatalog()->getPageCount()))
        fail("本文を編集するページを確認してください。");
    const auto root = document.getObject(document.getTrailerDictionary()->get("Root"));
    if (!root.isDictionary() || root.getDictionary()->hasKey("OCProperties") ||
        root.getDictionary()->hasKey("StructTreeRoot"))
        fail("レイヤーや構造タグがあるPDFの本文編集は未対応です。");
    const auto sourcePage = document.getCatalog()->getPage(page);
    Inspection result;
    result.bytes = contents(document, sourcePage->getContents());
    result.blocks = spans(result.bytes, cancelled);
    ContentInvocationOwner owner;
    owner.reference = sourcePage->getPageReference();
    owner.attributes = *document.getObjectByReference(owner.reference).getDictionary();
    if (const auto resources = document.getDictionaryFromObject(sourcePage->getResources()))
        owner.resources = *resources;
    owner.bytes = result.bytes;
    result.owners << owner;
    std::unique_ptr<FormContentInvocations> invocations;
    if (includeForms)
        invocations =
            std::make_unique<FormContentInvocations>(document, page, [&] { stop(cancelled); });
    PDFFontCache fonts{128, 128};
    auto snapshot = document;
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    Collector collector(PDFRenderer::getDefaultFeatures(), sourcePage, &document, &fonts,
                        cms.data(), nullptr, pageMatrix(sourcePage), {});
    collector.blocks = &result.blocks;
    collector.cancelled = cancelled;
    collector.invocations = invocations.get();
    collector.initialize();
    for (const auto& error : collector.processContents())
        if (error.type != RenderErrorType::Information)
            fail("本文の文字を解析できません: " + error.message);
    if (!collector.finished())
        fail("本文命令の対応を確認できません。");
    if (invocations)
    {
        result.owners.clear();
        for (const auto& tracked : invocations->owners)
            result.owners << tracked;
    }
    for (auto& block : result.blocks)
    {
        for (auto& part : block.parts)
        {
            part.value.depth = block.value.depth;
            const auto& restriction =
                layout == ExistingTextPartLayout::FlowingLine    ? block.flowRestriction
                : layout == ExistingTextPartLayout::FlowingLines ? block.linesRestriction
                                                                 : block.partsRestriction;
            if (!restriction.isEmpty())
                part.value.restriction = restriction;
            if (!part.font || part.fontName.isEmpty() ||
                (!part.value.text.isEmpty() && !part.value.physical.isValid()))
                part.value.restriction = "文字部分の字体・字形を確認できません。";
            if (part.font && part.value.font.isEmpty())
            {
                const auto fonts = document.getDictionaryFromObject(
                    result.owners[block.owner].resources.get("Font"));
                const auto font =
                    fonts ? document.getDictionaryFromObject(fonts->get(part.fontName)) : nullptr;
                const auto base = font ? document.getObject(font->get("BaseFont")) : PDFObject();
                part.value.font = base.isName() ? QString::fromLatin1(base.getString())
                                                : QString::fromLatin1(part.fontName);
            }
        }
        if (layout != ExistingTextPartLayout::Positioned)
        {
            QString restriction;
            for (const auto& part : block.parts)
            {
                if (!part.value.restriction.isEmpty())
                    restriction = part.value.restriction;
                if (!std::isfinite(part.origin.x()) || !std::isfinite(part.origin.y()) ||
                    !std::isfinite(part.end.x()) || !std::isfinite(part.end.y()) ||
                    !part.ctm.isInvertible())
                    restriction = "文字行の位置と字送りを確認できません。";
            }
            if (!block.value.text.isEmpty() &&
                !QRectF(QPointF(), pageSize(sourcePage)).contains(block.value.physical))
                restriction = "文字行全体がページの表示範囲からはみ出しています。";
            if (!restriction.isEmpty())
                for (auto& part : block.parts)
                    part.value.restriction = restriction;
        }
        if (!block.font || !block.value.physical.isValid() || !block.matrix.isInvertible() ||
            !block.ctm.isInvertible())
            block.value.restriction = "本文の字体・表示範囲・位置を確認できません。";
        if (block.font && !block.fontName.isEmpty() && block.value.font.isEmpty())
        {
            const auto fontsObject =
                document.getObject(result.owners[block.owner].resources.get("Font"));
            const auto fontObject =
                document.getObject(fontsObject.getDictionary()->get(block.fontName));
            const auto baseName = document.getObject(fontObject.getDictionary()->get("BaseFont"));
            block.value.font = baseName.isName() ? QString::fromLatin1(baseName.getString())
                                                 : QString::fromLatin1(block.fontName);
        }
    }
    return result;
}
QByteArray matrixBytes(const QTransform& matrix)
{
    QByteArray bytes;
    for (double value :
         {matrix.m11(), matrix.m12(), matrix.m21(), matrix.m22(), matrix.dx(), matrix.dy()})
    {
        if (!std::isfinite(value))
            fail("本文の座標が不正です。");
        bytes += QByteArray::number(value, 'f', 17) + ' ';
    }
    return bytes + "Tm";
}
PDFEncodedText encode(const Block& block, const QString& text)
{
    const auto& font = block.font;
    auto result = font->encodeText(text);
    // Standard fonts have no embedded GID array. Their encoding and known font
    // identity provide the codes; the candidate is still rendered and decoded.
    const auto simple = dynamic_cast<const PDFSimpleFont*>(font.data());
    if (result.isValid)
        return result;
    if (!simple || simple->getStandardFontType() == StandardFontType::Invalid)
    {
        // For a simple subset, re-use only exact codes observed in this block.
        // The renderer provided its Unicode characters; no encoding is guessed.
        const auto observed = QString(block.value.text).remove('\n');
        if (!simple || block.originalCodes.size() != observed.size())
            return result;
        result.encodedText.clear();
        result.isValid = true;
        for (auto character : text)
        {
            const auto position = observed.indexOf(character);
            if (position < 0)
            {
                result.isValid = false;
                break;
            }
            result.encodedText += block.originalCodes[position];
        }
        return result;
    }
    result.encodedText.clear();
    result.isValid = true;
    for (auto character : text)
    {
        bool found = false;
        const auto table = simple->getEncoding();
        for (size_t code = 0; code < table->size(); ++code)
            if ((*table)[code] == character)
            {
                result.encodedText += char(code);
                found = true;
                break;
            }
        if (!found)
            result.isValid = false;
    }
    return result;
}
} // namespace
QVector<ExistingTextBlock> existingTextBlocks(const PDFDocument& document, int page,
                                              const std::function<bool()>& cancelled,
                                              bool includeForms)
{
    QVector<ExistingTextBlock> result;
    for (const auto& block : inspect(document, page, cancelled, includeForms).blocks)
        if (!block.value.text.isEmpty())
            result << block.value;
    return result;
}
QVector<ExistingTextBlock> existingStyledTextParts(const PDFDocument& document, int page,
                                                   const std::function<bool()>& cancelled,
                                                   bool includeForms, ExistingTextPartLayout layout)
{
    QVector<ExistingTextBlock> result;
    int index = 0;
    for (const auto& block : inspect(document, page, cancelled, includeForms, layout).blocks)
        for (auto part : block.parts)
        {
            part.value.occurrence = index++;
            if (!part.value.text.isEmpty())
                result << part.value;
        }
    return result;
}
PDFDocument editExistingStyledTextPart(const PDFDocument& snapshot, int page, int occurrence,
                                       ExistingTextChange change, const QString& text,
                                       const std::function<bool()>& cancelled, bool includeForms,
                                       ExistingTextPartLayout layout)
{
    stop(cancelled);
    const auto restriction = editingRestriction(snapshot);
    if (!restriction.isEmpty())
        fail(restriction);
    if (change != ExistingTextChange::Replace && change != ExistingTextChange::Remove)
        fail("文字部分は元の字体を保持して置換・削除してください。");
    const auto inspected = inspect(snapshot, page, cancelled, includeForms, layout);
    int selectedBlock = -1, selectedPart = -1, index = 0;
    for (int b = 0; b < inspected.blocks.size(); ++b)
        for (int p = 0; p < inspected.blocks[b].parts.size(); ++p)
            if (index++ == occurrence)
            {
                selectedBlock = b;
                selectedPart = p;
            }
    if (selectedBlock < 0)
        fail("編集する文字部分が見つかりません。");
    const auto& block = inspected.blocks[selectedBlock];
    const auto& part = block.parts[selectedPart];
    if (!part.value.restriction.isEmpty())
        fail(part.value.restriction);
    QByteArray replacement = "[] TJ";
    if (change == ExistingTextChange::Replace)
    {
        if (text.isEmpty() || text.size() > 4096 || text.contains('\n') || text.contains('\r') ||
            text.contains(QChar::Null))
            fail("空でない4096文字以内の1行で文字部分を入力してください。");
        Block encoding;
        encoding.font = part.font;
        encoding.value.text = part.value.text;
        encoding.originalCodes = part.originalCodes;
        const auto encoded = encode(encoding, text);
        if (!encoded.isValid || encoded.encodedText.isEmpty())
            fail("元の字体で表現できない文字があります。文字部分は元の字体を保持します。");
        replacement = '<' + encoded.encodedText.toHex() + "> Tj";
    }
    const auto span = block.showSpans[selectedPart];
    PDFDocumentBuilder builder(&snapshot);
    spliceContentInvocation(builder, snapshot, inspected.owners, block.owner,
                            {span.begin, span.end}, replacement, [&] { stop(cancelled); });
    auto result = builder.build();
    const auto checked = inspect(result, page, cancelled, includeForms, layout);
    if (checked.blocks.size() != inspected.blocks.size())
        fail("変更後の本文構造を確認できません。");
    const auto pageBounds = QRectF(QPointF(), pageSize(snapshot.getCatalog()->getPage(page)));
    for (int b = 0; b < inspected.blocks.size(); ++b)
        if (checked.blocks[b].parts.size() != inspected.blocks[b].parts.size())
            fail("変更後の文字部分の構造を確認できません。");
    const auto newBounds = checked.blocks[selectedBlock].parts[selectedPart].value.physical;
    const auto& changed = checked.blocks[selectedBlock].parts[selectedPart].value;
    const bool flowing = layout != ExistingTextPartLayout::Positioned;
    if ((flowing || change != ExistingTextChange::Remove) && !changed.restriction.isEmpty())
        fail(changed.restriction);
    const auto delta = checked.blocks[selectedBlock].parts[selectedPart].end - part.end;
    const auto close = [](double a, double b) { return std::abs(a - b) <= 1e-8; };
    const auto samePoint = [&](QPointF a, QPointF b)
    { return close(a.x(), b.x()) && close(a.y(), b.y()); };
    const auto sameBox = [&](QRectF a, QRectF b)
    {
        return close(a.left(), b.left()) && close(a.top(), b.top()) &&
               close(a.right(), b.right()) && close(a.bottom(), b.bottom());
    };
    const auto lineBounds = [](const Block& value, int line)
    {
        QRectF bounds;
        for (const auto& item : value.parts)
            if (item.value.physicalLine == line)
                bounds = bounds.united(item.value.physical);
        return bounds;
    };
    const auto oldLine = lineBounds(block, part.value.physicalLine);
    const auto newLine = lineBounds(checked.blocks[selectedBlock], part.value.physicalLine);
    for (int b = 0; b < inspected.blocks.size(); ++b)
    {
        const auto& before = inspected.blocks[b].parts;
        const auto& after = checked.blocks[b].parts;
        if (before.size() != after.size())
            fail("変更後の文字部分の構造を確認できません。");
        for (int p = 0; p < before.size(); ++p)
        {
            const bool selected = b == selectedBlock && p == selectedPart;
            const bool following = flowing && b == selectedBlock && p > selectedPart &&
                                   before[p].value.physicalLine == part.value.physicalLine;
            const auto expected = selected
                                      ? (change == ExistingTextChange::Remove ? QString() : text)
                                      : before[p].value.text;
            if (after[p].value.text != expected || after[p].value.font != before[p].value.font ||
                after[p].value.physicalLine != before[p].value.physicalLine ||
                (!selected && !following && after[p].value.physical != before[p].value.physical))
                fail("変更後の文字・字体・未選択位置が一致しません。変更は適用していません。");
            if (flowing)
            {
                const auto translation = following ? delta : QPointF();
                const auto& oldPart = before[p];
                const auto& newPart = after[p];
                if (oldPart.ctm != newPart.ctm || oldPart.fontSize != newPart.fontSize ||
                    oldPart.matrix.m11() != newPart.matrix.m11() ||
                    oldPart.matrix.m12() != newPart.matrix.m12() ||
                    oldPart.matrix.m21() != newPart.matrix.m21() ||
                    oldPart.matrix.m22() != newPart.matrix.m22() ||
                    !samePoint(newPart.origin, oldPart.origin + translation) ||
                    (!selected && (!samePoint(newPart.end, oldPart.end + translation) ||
                                   oldPart.glyphs.size() != newPart.glyphs.size())))
                    fail(
                        "変更後の文字行の字送り・字体状態が一致しません。変更は適用していません。");
                if (!selected)
                    for (int g = 0; g < oldPart.glyphs.size(); ++g)
                        if (!sameBox(newPart.glyphs[g], oldPart.glyphs[g].translated(translation)))
                            fail("後続文字の字形が字送りに一致しません。変更は適用していません。");
            }
            if (!flowing && !selected && change == ExistingTextChange::Replace &&
                !part.value.physical.intersects(before[p].value.physical) &&
                newBounds.intersects(after[p].value.physical))
                fail("置換した文字が別の文字部分と重なります。短い文字列を指定してください。");
        }
        if (flowing)
            for (int line = 0; line <= inspected.blocks[b].physicalLine; ++line)
                if ((b != selectedBlock || line != part.value.physicalLine) &&
                    !oldLine.intersects(lineBounds(inspected.blocks[b], line)) &&
                    newLine.intersects(lineBounds(checked.blocks[b], line)))
                    fail("字送り後の文字行が別の本文と重なります。短い文字列を指定してください。");
    }
    if (change != ExistingTextChange::Remove && !pageBounds.contains(newBounds))
        fail("変更後の文字部分の字形・ページ範囲を確認できません。変更は適用していません。");
    stop(cancelled);
    return result;
}
static PDFDocument editText(const PDFDocument& snapshot, int page, int occurrence,
                            ExistingTextChange change, const QString& text, QRectF physical,
                            const std::function<bool()>& cancelled, const QString& family,
                            double leadingRatio = 0, bool includeForms = false)
{
    const auto restriction = editingRestriction(snapshot);
    if (!restriction.isEmpty())
        fail(restriction);
    if (change != ExistingTextChange::Geometry && change != ExistingTextChange::Replace &&
        change != ExistingTextChange::Remove && change != ExistingTextChange::Lines)
        fail("本文の編集操作を確認してください。");
    const auto inspected = inspect(snapshot, page, cancelled, includeForms);
    if (occurrence < 0 || occurrence >= inspected.blocks.size())
        fail("編集対象の本文が見つかりません。");
    const auto& block = inspected.blocks[occurrence];
    if (!block.value.restriction.isEmpty())
        fail(block.value.restriction);
    auto owners = inspected.owners;
    auto& owner = owners[block.owner];
    QByteArray bytes = owner.bytes;
    PDFDocumentBuilder builder(&snapshot);
    const auto sourcePage = snapshot.getCatalog()->getPage(page);
    auto dictionary = owner.attributes;
    if (change == ExistingTextChange::Geometry)
    {
        if (!physical.isValid() || !std::isfinite(physical.left()) ||
            !std::isfinite(physical.top()) || !std::isfinite(physical.right()) ||
            !std::isfinite(physical.bottom()) || physical.width() < 1 || physical.height() < 1 ||
            !QRectF(QPointF(), pageSize(snapshot.getCatalog()->getPage(page))).contains(physical))
            fail("本文の位置と大きさをページ範囲内で指定してください。");
        const auto transform = pageMatrix(snapshot.getCatalog()->getPage(page));
        const auto source = block.value.physical;
        const double sx = physical.width() / source.width(),
                     sy = physical.height() / source.height();
        const QTransform delta(sx, 0, 0, sy, physical.x() - sx * source.x(),
                               physical.y() - sy * source.y());
        const auto changed = block.matrix * block.ctm * transform * delta * transform.inverted() *
                             block.ctm.inverted();
        bytes.replace(block.matrixSpan.begin, block.matrixSpan.end - block.matrixSpan.begin,
                      matrixBytes(changed));
    }
    else
    {
        QVector<QByteArray> commands(block.shows, "[] TJ");
        QByteArray prefix, suffix;
        if (change == ExistingTextChange::Replace || change == ExistingTextChange::Lines)
        {
            if (text.isEmpty() || text.size() > 4096 || text.contains('\r') ||
                text.contains(QChar::Null))
                fail(change == ExistingTextChange::Lines
                         ? QString("空でない1〜64行、4096文字以内の本文を入力してください。")
                         : QString("元の本文と同じ%"
                                   "1行で、空でない4096文字以内の本文を入力してください。")
                               .arg(block.shows));
            const auto lines = text.split('\n');
            if (change != ExistingTextChange::Lines && lines.size() != block.shows)
                fail(QString("元の本文と同じ%1行で入力してください。行数の変更は未対応です。")
                         .arg(block.shows));
            if (lines.contains(QString()))
                fail("各行に文字を入力してください。空行への変更は未対応です。");
            if (change == ExistingTextChange::Lines)
            {
                if (lines.size() > 64 || !std::isfinite(leadingRatio) || leadingRatio < .5 ||
                    leadingRatio > 4 || !std::isfinite(block.fontSize) || block.fontSize <= 0 ||
                    !std::isfinite(block.endingLeading))
                    fail("64行以内の本文と0.5〜4倍の行送りを指定してください。");
                commands.resize(lines.size());
            }
            if (family.isEmpty())
            {
                for (int i = 0; i < lines.size(); ++i)
                {
                    const auto encoded = encode(block, lines[i]);
                    if (!encoded.isValid || encoded.encodedText.isEmpty())
                        fail("元の字体で表現できない文字があります。本文の書体を選んでください。");
                    commands[i] = '<' + encoded.encodedText.toHex() + "> Tj";
                }
            }
            else
            {
                if (!std::isfinite(block.fontSize) || block.fontSize <= 0)
                    fail("本文の文字サイズを確認できません。");
                const auto embedded = embedBodyTextFont(builder, family, text, cancelled);
                auto resources = owner.resources;
                auto fonts = *snapshot.getDictionaryFromObject(resources.get("Font"));
                int number = 1;
                QByteArray name;
                do
                {
                    name = "TatsujinBodyFont" + QByteArray::number(number++);
                } while (fonts.hasKey(name));
                fonts.setEntry(PDFInplaceOrMemoryString(name), PDFObject(embedded.font));
                set(resources, "Font", dictObject(fonts));
                set(dictionary, "Resources", dictObject(resources));
                owner.resources = resources;
                QByteArray originalName;
                for (unsigned char character : block.fontName)
                {
                    if (character >= 33 && character <= 126 &&
                        !QByteArray("#/%()<>[]{}").contains(char(character)))
                        originalName += char(character);
                    else
                        originalName +=
                            '#' + QByteArray::number(character, 16).rightJustified(2, '0');
                }
                if (originalName.isEmpty() || !fonts.hasKey(block.fontName))
                    fail("元の字体リソースを確認できません。");
                const auto size = QByteArray::number(block.fontSize, 'f', 17);
                prefix = '/' + name + ' ' + size + " Tf\n";
                suffix = "\n/" + originalName + ' ' + size + " Tf";
                for (int i = 0; i < embedded.encodedLines.size(); ++i)
                    commands[i] = '<' + embedded.encodedLines[i].toHex() + "> Tj";
            }
        }
        commands.first().prepend(prefix);
        commands.last().append(suffix);
        if (change == ExistingTextChange::Lines)
        {
            const auto leading = QByteArray::number(block.fontSize * leadingRatio, 'f', 17);
            QByteArray replacement = leading + " TL\n";
            for (int i = 0; i < commands.size(); ++i)
            {
                if (i)
                    replacement += "\nT*\n";
                replacement += commands[i];
            }
            replacement += "\n" + QByteArray::number(block.endingLeading, 'f', 17) + " TL";
            bytes.replace(block.showSpans.first().begin,
                          block.showSpans.last().end - block.showSpans.first().begin, replacement);
        }
        else
            for (int i = commands.size() - 1; i >= 0; --i)
                bytes.replace(block.showSpans[i].begin,
                              block.showSpans[i].end - block.showSpans[i].begin, commands[i]);
    }
    stop(cancelled);
    owner.attributes = dictionary;
    const ContentSpan complete{0, owner.bytes.size()};
    spliceContentInvocation(builder, snapshot, std::move(owners), block.owner, complete, bytes,
                            [&] { stop(cancelled); });
    auto result = builder.build();
    const auto checked = inspect(result, page, cancelled, includeForms);
    if (checked.blocks.size() != inspected.blocks.size())
        fail("変更後の本文構造を確認できません。");
    for (int index = 0; index < checked.blocks.size(); ++index)
    {
        const auto expected = index == occurrence && change != ExistingTextChange::Geometry
                                  ? (change == ExistingTextChange::Remove ? QString() : text)
                                  : inspected.blocks[index].value.text;
        if (checked.blocks[index].value.text != expected)
            fail("変更後の本文の文字コードが一致しません。変更は適用していません。");
    }
    if (change != ExistingTextChange::Remove &&
        !checked.blocks[occurrence].value.restriction.isEmpty())
        fail(checked.blocks[occurrence].value.restriction);
    if (change != ExistingTextChange::Remove &&
        !QRectF(QPointF(), pageSize(sourcePage))
             .contains(checked.blocks[occurrence].value.physical))
        fail("変更後の本文がページからはみ出します。変更は適用していません。");
    stop(cancelled);
    return result;
}
PDFDocument editExistingText(const PDFDocument& snapshot, int page, int occurrence,
                             ExistingTextChange change, const QString& text, QRectF physical,
                             const std::function<bool()>& cancelled, bool includeForms)
{
    return editText(snapshot, page, occurrence, change, text, physical, cancelled, {}, 0,
                    includeForms);
}
PDFDocument replaceExistingTextFont(const PDFDocument& snapshot, int page, int occurrence,
                                    const QString& text, const QString& family,
                                    const std::function<bool()>& cancelled, bool includeForms)
{
    if (family.isEmpty())
        fail("本文の書体を明示的に選んでください。");
    return editText(snapshot, page, occurrence, ExistingTextChange::Replace, text, {}, cancelled,
                    family, 0, includeForms);
}
PDFDocument replaceExistingTextLines(const PDFDocument& snapshot, int page, int occurrence,
                                     const QString& text, double leadingRatio,
                                     const QString& family, const std::function<bool()>& cancelled,
                                     bool includeForms)
{
    return editText(snapshot, page, occurrence, ExistingTextChange::Lines, text, {}, cancelled,
                    family, leadingRatio, includeForms);
}
PDFDocument replaceExistingTextWrapped(const PDFDocument& snapshot, int page, int occurrence,
                                       const QString& text, double width, double leadingRatio,
                                       const QString& family,
                                       const std::function<bool()>& cancelled, bool includeForms)
{
    const auto inspected = inspect(snapshot, page, cancelled, includeForms);
    if (occurrence < 0 || occurrence >= inspected.blocks.size())
        fail("編集対象の本文が見つかりません。");
    const auto& block = inspected.blocks[occurrence];
    if (!block.value.restriction.isEmpty())
        fail(block.value.restriction);
    const auto transform =
        block.matrix * block.ctm * pageMatrix(snapshot.getCatalog()->getPage(page));
    if (std::abs(transform.m12()) > 1e-10 || std::abs(transform.m21()) > 1e-10 ||
        transform.m11() <= 0 || !std::isfinite(transform.m11()))
        fail("回転や傾斜のない横書き本文を選んでください。");
    PDFFontPointer font = block.font;
    const auto& owner = inspected.owners[block.owner];
    const auto sourceFonts = snapshot.getDictionaryFromObject(owner.resources.get("Font"));
    if (!sourceFonts)
        fail("本文の字体資源を確認できません。");
    const auto sourceFont = snapshot.getDictionaryFromObject(sourceFonts->get(block.fontName));
    if (!sourceFont)
        fail("本文の字体辞書を確認できません。");
    const auto sourceBase = snapshot.getObject(sourceFont->get("BaseFont"));
    const auto sourceEncoding = snapshot.getObject(sourceFont->get("Encoding"));
    const QByteArray baseFont = sourceBase.isName() ? sourceBase.getString() : QByteArray();
    const QByteArray encoding = sourceEncoding.isName() ? sourceEncoding.getString() : QByteArray();
    PDFDocument fontDocument;
    if (!family.isEmpty())
    {
        PDFDocumentBuilder builder(&snapshot);
        const auto embedded = embedBodyTextFont(builder, family, text, cancelled);
        fontDocument = builder.build();
        font = PDFFont::createFont(embedded.font, "TatsujinWrapFont", &fontDocument);
    }
    PDFRenderErrorReporterDummy reporter;
    const auto realized = PDFRealizedFont::createRealizedFont(font, block.fontSize, &reporter);
    if (!realized || !realized->isHorizontalWritingSystem())
        fail("本文の横書き字体を確認できません。");
    const auto wrapped = wrapBodyText(
        text, width,
        [&](const QString& paragraph)
        {
            stop(cancelled);
            const auto encoded =
                family.isEmpty() ? encode(block, paragraph) : font->encodeText(paragraph);
            if (!encoded.isValid)
                fail("元の字体で表現できない文字があります。本文の書体を選んでください。");
            TextSequence sequence;
            realized->fillTextSequence(encoded.encodedText, sequence, &reporter);
            QVector<BodyGlyphExtent> glyphs;
            QString actual;
            for (const auto& item : sequence.items)
            {
                if (!item.isCharacter() || item.isContentStream() || item.character.isNull())
                    fail("本文の字形を確認できません。");
                actual += item.character;
                const auto bounds = item.glyph->boundingRect();
                // PDF advances are independent of FreeType's 26.6 raster rounding.
                // Use the logical /Widths or CID /W values at the source font size.
                const auto logicalAdvance = pdfGlyphAdvance(font, item.cid, baseFont, encoding);
                glyphs << BodyGlyphExtent{logicalAdvance * .001 * block.fontSize * transform.m11(),
                                          bounds.left() * transform.m11(),
                                          bounds.right() * transform.m11()};
            }
            if (actual != paragraph)
                fail("本文の字形と文字コードが一致しません。");
            return glyphs;
        },
        cancelled);
    const auto result = replaceExistingTextLines(snapshot, page, occurrence, wrapped, leadingRatio,
                                                 family, cancelled, includeForms);
    const auto checked = inspect(result, page, cancelled, includeForms);
    if (checked.blocks[occurrence].value.physical.width() > width + .5)
        fail("折返し後の本文が指定幅を超えています。変更は適用していません。");
    return result;
}
} // namespace tatsu
