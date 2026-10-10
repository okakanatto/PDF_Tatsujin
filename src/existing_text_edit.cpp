#include "existing_text_edit.h"
#include "body_text_font.h"
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
struct Block
{
    ExistingTextBlock value;
    Span matrixSpan;
    QVector<Span> showSpans;
    int matrices = 0, shows = 0, advances = 0;
    bool stateChanged = false;
    QByteArray advanceCommand;
    double advanceDy = 0;
    QTransform matrix, ctm;
    PDFFontPointer font;
    double fontSize = 0;
    double endingLeading = 0;
    QByteArray originalCodes;
};
struct Inspection
{
    QByteArray bytes;
    QVector<Block> blocks;
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
                blocks.last().originalCodes += token.data.toByteArray();
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
            }
            else if (command == "Tj" || command == "TJ")
            {
                if (block.shows && (block.advances != block.shows || block.stateChanged))
                    block.value.restriction = "一定の字体と行送りを持つ複数行を選んでください。";
                block.showSpans << span;
                ++block.shows;
                block.stateChanged = false;
            }
            else if (command == "T*" || command == "Td" || command == "TD")
            {
                if (!block.shows || block.advances != block.shows - 1)
                    block.value.restriction = "各文字描画の間に1回改行する本文を選んでください。";
                const bool relative = command != "T*";
                const bool valid =
                    numericOperands && (relative ? numbers.size() == 2 && numbers[0] == 0 &&
                                                       std::isfinite(numbers[1]) && numbers[1] < 0
                                                 : numbers.isEmpty());
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
                block.value.restriction =
                    "複数行や非対応の本文命令を含むブロックは編集できません。";
            else if (block.shows)
                block.stateChanged = true;
        }
        operand = -1;
        numbers.clear();
        numericOperands = true;
    }
    if (inside || arrays)
        fail("本文の文字ブロックが閉じられていません。");
    for (auto& block : blocks)
        if (block.matrices != 1 || block.shows < 1 || block.shows > 64 ||
            block.advances != block.shows - 1 ||
            block.matrixSpan.end > block.showSpans.first().begin)
            block.value.restriction =
                "1個の位置行列と一定の行送りを持つ64行以内のブロックを選んでください。";
    return blocks;
}
class Collector : public PDFTextLayoutGenerator
{
public:
    using PDFTextLayoutGenerator::PDFTextLayoutGenerator;
    QVector<Block>* blocks = nullptr;
    std::function<bool()> cancelled;
    int index = -1;
    bool inside = false;
    int line = 0;
    bool firstCharacter = false;

protected:
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        return kind == ContentKind::Forms || kind == ContentKind::Images ||
               PDFTextLayoutGenerator::isContentKindSuppressed(kind);
    }
    void performInterceptInstruction(Operator, ProcessOrder order,
                                     const QByteArray& command) override
    {
        stop(cancelled);
        if (order != ProcessOrder::BeforeOperation)
            return;
        if (command == "BT")
        {
            if (++index >= blocks->size())
                fail("本文命令の対応を確認できません。");
            inside = true;
            line = 0;
        }
        else if (command == "ET")
            inside = false;
        else if (inside && (command == "Tj" || command == "TJ"))
        {
            auto& block = (*blocks)[index];
            const auto state = getGraphicState();
            block.endingLeading = state->getTextLeading();
            if (block.shows > 1 && block.advanceCommand == "T*" &&
                (!std::isfinite(state->getTextLeading()) || state->getTextLeading() <= 0))
                block.value.restriction = "正の一定行送りを持つ複数行を選んでください。";
            firstCharacter = true;
            if (!line++)
            {
                block.font = state->getTextFont();
                block.fontSize = state->getTextFontSize();
                block.matrix = state->getTextMatrix();
                block.ctm = state->getCurrentTransformationMatrix();
                if (block.font)
                    block.value.font =
                        QString::fromLatin1(block.font->getFontDescriptor()->fontName);
            }
            if (state->getTextRenderingMode() != TextRenderingMode::Fill ||
                state->getAlphaFilling() != 1 || isContentSuppressed())
                block.value.restriction =
                    "不可視OCR・文字クリップ・透過文字の本文編集は未対応です。";
        }
    }
    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        if (!inside || isContentSuppressed())
            return;
        auto& block = (*blocks)[index];
        if (firstCharacter && line > 1 && !block.value.text.isEmpty())
            block.value.text += '\n';
        firstCharacter = false;
        block.value.text += info.character;
        if (block.value.text.size() > 4096)
            fail("本文ブロックの文字数が上限を超えています。");
        if (info.isVerticalWritingSystem)
            block.value.restriction = "縦書き本文の編集は未対応です。";
        const auto box =
            getPagePointToDevicePointMatrix().map(info.matrix.map(info.outline)).boundingRect();
        if (box.isValid())
            block.value.physical = block.value.physical.united(box);
    }
};
Inspection inspect(const PDFDocument& document, int page, const std::function<bool()>& cancelled)
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
    PDFFontCache fonts{128, 128};
    auto snapshot = document;
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    Collector collector(PDFRenderer::getDefaultFeatures(), sourcePage, &document, &fonts,
                        cms.data(), nullptr, pageMatrix(sourcePage), {});
    collector.blocks = &result.blocks;
    collector.cancelled = cancelled;
    for (const auto& error : collector.processContents())
        if (error.type != RenderErrorType::Information)
            fail("本文の文字を解析できません: " + error.message);
    if (collector.index + 1 != result.blocks.size())
        fail("本文命令の対応を確認できません。");
    for (auto& block : result.blocks)
    {
        if (!block.font || !block.value.physical.isValid() || !block.matrix.isInvertible() ||
            !block.ctm.isInvertible())
            block.value.restriction = "本文の字体・表示範囲・位置を確認できません。";
        if (block.font && block.value.font.isEmpty())
        {
            const auto resources = document.getObject(sourcePage->getResources());
            const auto fontsObject = document.getObject(resources.getDictionary()->get("Font"));
            const auto fontObject =
                document.getObject(fontsObject.getDictionary()->get(block.font->getFontId()));
            const auto baseName = document.getObject(fontObject.getDictionary()->get("BaseFont"));
            block.value.font = baseName.isName() ? QString::fromLatin1(baseName.getString())
                                                 : QString::fromLatin1(block.font->getFontId());
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
                                              const std::function<bool()>& cancelled)
{
    QVector<ExistingTextBlock> result;
    for (const auto& block : inspect(document, page, cancelled).blocks)
        if (!block.value.text.isEmpty())
            result << block.value;
    return result;
}
static PDFDocument editText(const PDFDocument& snapshot, int page, int occurrence,
                            ExistingTextChange change, const QString& text, QRectF physical,
                            const std::function<bool()>& cancelled, const QString& family,
                            double leadingRatio = 0)
{
    const auto restriction = editingRestriction(snapshot);
    if (!restriction.isEmpty())
        fail(restriction);
    if (change != ExistingTextChange::Geometry && change != ExistingTextChange::Replace &&
        change != ExistingTextChange::Remove && change != ExistingTextChange::Lines)
        fail("本文の編集操作を確認してください。");
    const auto inspected = inspect(snapshot, page, cancelled);
    if (occurrence < 0 || occurrence >= inspected.blocks.size())
        fail("編集対象の本文が見つかりません。");
    const auto& block = inspected.blocks[occurrence];
    if (!block.value.restriction.isEmpty())
        fail(block.value.restriction);
    QByteArray bytes = inspected.bytes;
    PDFDocumentBuilder builder(&snapshot);
    const auto sourcePage = snapshot.getCatalog()->getPage(page);
    auto dictionary =
        *snapshot.getObjectByReference(sourcePage->getPageReference()).getDictionary();
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
                auto resources = *snapshot.getDictionaryFromObject(sourcePage->getResources());
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
                QByteArray originalName;
                for (unsigned char character : block.font->getFontId())
                {
                    if (character >= 33 && character <= 126 &&
                        !QByteArray("#/%()<>[]{}").contains(char(character)))
                        originalName += char(character);
                    else
                        originalName +=
                            '#' + QByteArray::number(character, 16).rightJustified(2, '0');
                }
                if (originalName.isEmpty() || !fonts.hasKey(block.font->getFontId()))
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
    set(dictionary, "Contents",
        PDFObject::createReference(builder.addObject(streamObject({}, bytes))));
    builder.setObject(sourcePage->getPageReference(), dictObject(dictionary));
    auto result = builder.build();
    const auto checked = inspect(result, page, cancelled);
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
        !QRectF(QPointF(), pageSize(sourcePage))
             .contains(checked.blocks[occurrence].value.physical))
        fail("変更後の本文がページからはみ出します。変更は適用していません。");
    stop(cancelled);
    return result;
}
PDFDocument editExistingText(const PDFDocument& snapshot, int page, int occurrence,
                             ExistingTextChange change, const QString& text, QRectF physical,
                             const std::function<bool()>& cancelled)
{
    return editText(snapshot, page, occurrence, change, text, physical, cancelled, {});
}
PDFDocument replaceExistingTextFont(const PDFDocument& snapshot, int page, int occurrence,
                                    const QString& text, const QString& family,
                                    const std::function<bool()>& cancelled)
{
    if (family.isEmpty())
        fail("本文の書体を明示的に選んでください。");
    return editText(snapshot, page, occurrence, ExistingTextChange::Replace, text, {}, cancelled,
                    family);
}
PDFDocument replaceExistingTextLines(const PDFDocument& snapshot, int page, int occurrence,
                                     const QString& text, double leadingRatio,
                                     const QString& family, const std::function<bool()>& cancelled)
{
    return editText(snapshot, page, occurrence, ExistingTextChange::Lines, text, {}, cancelled,
                    family, leadingRatio);
}
} // namespace tatsu
