#include "redaction_content.h"
#include "image_embedding.h"
#include "pdf_objects.h"
#include "pdfcms.h"
#include "pdfdocumentwriter.h"
#include "pdffont.h"
#include "pdfpagecontentprocessor.h"
#include "pdfparser.h"
#include "standard_font_metrics.h"
#include <cmath>
#include <exception>

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
class ContentProcessor final : public PDFPageContentProcessor
{
public:
    ContentProcessor(const PDFPage* page, const PDFDocument* document, const PDFFontCache* fonts,
                     const PDFCMS* cms, QPainterPath regions, PDFDocumentBuilder& builder,
                     std::function<bool()> cancelled)
        : PDFPageContentProcessor(page, document, fonts, cms, nullptr, {}, {}),
          regions(std::move(regions)), builder(builder), cancelled(std::move(cancelled))
    {
        result.bytes = "q\n";
    }
    RedactedPageContent finish()
    {
        if (deferredError)
            std::rethrow_exception(deferredError);
        stop();
        if (graphicsDepth != 0)
            fail("描画状態の対応が不正です。墨消ししていません。");
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
            if (op == Operator::Invalid || name == "'" || name == "\"" || name == "sh" ||
                name == "gs" || name == "cs" || name == "CS" || name == "scn" || name == "SCN" ||
                name == "BMC" || name == "BDC" || name == "EMC" || name == "MP" || name == "DP" ||
                name == "BX" || name == "EX")
                fail("この内容命令を含むPDFの墨消しにはまだ対応していません: " +
                     QString::fromLatin1(name));
            command = name;
            serialized.clear();
            selectedCharacters = unselectedCharacters = 0;
            exactAdvance = 0;
            replacementImage.clear();
            if (name == "q")
                ++graphicsDepth;
            if (name == "Q" && --graphicsDepth < 0)
                fail("描画状態の対応が不正です。");
            for (size_t i = 0; i < getOperands().size(); ++i)
                serialized += tokenBytes(getOperands()[i]) + " ";
            if (name == "Tf")
            {
                if (getOperands().size() != 2 || !getFontDictionary())
                    fail("書体の参照が不正です。");
                const auto fontName = getOperands()[0].data.toByteArray();
                result.fonts.setEntry(PDFInplaceOrMemoryString(fontName),
                                      PDFObject(getFontDictionary()->get(fontName)));
            }
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
            if (selectedCharacters)
            {
                // Do not silently remove text outside the selected rectangle.
                // Partial operations require a future character-code splitter.
                if (unselectedCharacters || (command != "Tj" && command != "TJ"))
                    fail("文字命令の一部だけの墨消しにはまだ対応していません。出力していません。");
                result.bytes += "[" + numeric(exactAdvance) + "] TJ\n";
                ++result.removedTextSegments;
            }
            else if (!replacementImage.isEmpty())
                result.bytes += PDFDocumentWriter::getSerializedObject(
                                    PDFObject::createName(replacementImage)) +
                                " Do\n";
            else
            {
                result.bytes += serialized + command + "\n";
                if (command == "Do")
                    result.xobjects.setEntry(PDFInplaceOrMemoryString(imageName),
                                             PDFObject(getXObjectDictionary()->get(imageName)));
            }
            if (result.bytes.size() > 64 * 1024 * 1024)
                fail("墨消し候補の内容が処理上限を超えています。");
        }
    }
    void performProcessTextSequence(const TextSequence& sequence, ProcessOrder order) override
    {
        if (order != ProcessOrder::BeforeOperation)
            return;
        const auto font = getGraphicState()->getTextFont();
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
            if (item.glyph && item.character.isNull())
                fail("文字対応を検証できないため墨消ししていません。");
        const auto state = getGraphicState();
        if (state->getTextFontSize() <= 0)
            fail("ゼロや負の文字サイズの墨消しは未評価です。");
        float displacement = 0;
        const float size = float(state->getTextFontSize());
        for (const auto& item : sequence.items)
        {
            if (!item.glyph)
                displacement -= float(item.advance) * size / 1000;
            else
            {
                const double spacing =
                    state->getTextCharacterSpacing() +
                    (item.character == QChar(' ') ? state->getTextWordSpacing() : 0);
                displacement += float(glyphWidth(item.cid)) * size / 1000 + float(spacing);
            }
        }
        // Retain normal PDF renderer float accumulation. Emit only the total
        // advance, so removed characters leave no individual-width sequence.
        exactAdvance = -double(displacement) * 1000 / state->getTextFontSize();
    }
    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        if (info.character.isSpace())
            return;
        auto bounds = info.matrix.map(info.outline).boundingRect();
        if (bounds.isEmpty())
            fail("文字の範囲を確認できません。");
        if (regions.intersects(bounds))
        {
            if (!regions.contains(bounds))
                fail("文字の一部を横切る墨消しにはまだ対応していません。");
            ++selectedCharacters;
        }
        else
            ++unselectedCharacters;
    }
    void performPathPainting(const QPainterPath& path, bool stroke, bool fill, bool text,
                             Qt::FillRule) override
    {
        Q_UNUSED(path);
        if (!text && (stroke || fill))
            fail("描画パスを含むページの安全な墨消しは未評価です。");
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
        for (int y = limit.top(); y <= limit.bottom(); ++y)
        {
            stop();
            for (int x = limit.left(); x <= limit.right(); ++x)
                if (mask.intersects(QRectF(x, y, 1, 1)))
                    image.setPixel(x, y, qRgb(0, 0, 0));
        }
        int suffix = result.modifiedImages + 1;
        const auto objects = getXObjectDictionary();
        do
            replacementImage = "TatsujinRedactImage" + QByteArray::number(suffix++);
        while ((objects && objects->hasKey(replacementImage)) ||
               result.xobjects.hasKey(replacementImage));
        result.xobjects.setEntry(PDFInplaceOrMemoryString(replacementImage),
                                 PDFObject::createReference(embedImage(builder, image)));
        ++result.modifiedImages;
    }

private:
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
        const auto font = getGraphicState()->getTextFont();
        const auto declaration =
            getDocument()->getDictionaryFromObject(getFontDictionary()->get(font->getFontId()));
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
    RedactedPageContent result;
    QByteArray command, serialized, imageName, replacementImage;
    double exactAdvance = 0;
    std::exception_ptr deferredError;
    int graphicsDepth = 0, selectedCharacters = 0, unselectedCharacters = 0;
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
RedactedPageContent redactPageContent(const PDFDocument& document, int page,
                                      const QVector<QRectF>& rectangles,
                                      PDFDocumentBuilder& builder,
                                      const std::function<bool()>& cancelled)
{
    auto regions = checkedRedactionRegions(document, page, rectangles);
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
    else
        inspect(contents);
    auto snapshot = document;
    PDFFontCache fonts{128, 128};
    fonts.setDocument(PDFModifiedDocument(&snapshot, nullptr));
    PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    ContentProcessor processor(pdfPage, &document, &fonts, cms.data(), regions, builder, cancelled);
    processor.rectangles = rectangles;
    for (const auto& error : processor.processContents())
        if (error.type != RenderErrorType::Information)
            fail("墨消し前の内容解析に失敗しました: " + error.message);
    return processor.finish();
}
} // namespace tatsu
