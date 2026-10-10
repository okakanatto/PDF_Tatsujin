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
    Span matrixSpan, showSpan;
    int matrices = 0, shows = 0;
    QTransform matrix, ctm;
    PDFFontPointer font;
    double fontSize = 0;
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
                block.showSpan = span;
                ++block.shows;
            }
            else if (command != "Tf" && command != "Tc" && command != "Tw" && command != "Tz" &&
                     command != "TL" && command != "Tr" && command != "Ts")
                block.value.restriction =
                    "複数行や非対応の本文命令を含むブロックは編集できません。";
        }
        operand = -1;
    }
    if (inside || arrays)
        fail("本文の文字ブロックが閉じられていません。");
    for (auto& block : blocks)
        if (block.matrices != 1 || block.shows != 1 || block.matrixSpan.end > block.showSpan.begin)
            block.value.restriction =
                "1個の位置行列と1回の文字描画を持つブロックを選んでください。";
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
        }
        else if (command == "ET")
            inside = false;
        else if (inside && (command == "Tj" || command == "TJ"))
        {
            auto& block = (*blocks)[index];
            const auto state = getGraphicState();
            block.font = state->getTextFont();
            block.fontSize = state->getTextFontSize();
            block.matrix = state->getTextMatrix();
            block.ctm = state->getCurrentTransformationMatrix();
            if (block.font)
                block.value.font = QString::fromLatin1(block.font->getFontDescriptor()->fontName);
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
        if (!block.font || !block.value.physical.isValid() || !block.matrix.isInvertible() ||
            !block.ctm.isInvertible())
            block.value.restriction = "本文の字体・表示範囲・位置を確認できません。";
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
        if (!simple || block.originalCodes.size() != block.value.text.size())
            return result;
        result.encodedText.clear();
        result.isValid = true;
        for (auto character : text)
        {
            const auto position = block.value.text.indexOf(character);
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
                            const std::function<bool()>& cancelled, const QString& family)
{
    const auto restriction = editingRestriction(snapshot);
    if (!restriction.isEmpty())
        fail(restriction);
    if (change != ExistingTextChange::Geometry && change != ExistingTextChange::Replace &&
        change != ExistingTextChange::Remove)
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
        QByteArray command = "[] TJ";
        if (change == ExistingTextChange::Replace)
        {
            if (text.isEmpty() || text.size() > 4096 || text.contains('\n') ||
                text.contains('\r') || text.contains(QChar::Null))
                fail("置換する本文は空でない1行、4096文字以内で指定してください。");
            if (family.isEmpty())
            {
                const auto encoded = encode(block, text);
                if (!encoded.isValid || encoded.encodedText.isEmpty())
                    fail("元の字体で表現できない文字があります。本文の書体を選んでください。");
                command = '<' + encoded.encodedText.toHex() + "> Tj";
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
                command = '/' + name + ' ' + size + " Tf\n<" + embedded.encodedText.toHex() +
                          "> Tj\n/" + originalName + ' ' + size + " Tf";
            }
        }
        bytes.replace(block.showSpan.begin, block.showSpan.end - block.showSpan.begin, command);
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
} // namespace tatsu
