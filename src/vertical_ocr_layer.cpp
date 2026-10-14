#include "vertical_ocr_layer.h"
#include "ocr_column_font.h"
#include "pdf_font_metrics.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include <algorithm>
#include <cmath>
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

namespace tatsu
{
namespace
{
struct Symbol
{
    QString text;
    QRectF box;
    bool columnEnd = false;
};
QString decimal(double value)
{
    return QString::number(value, 'g', 15);
}
QVector<Symbol> columnSymbols(const QString& text, QRect word, const QImage& image)
{
    const auto characters = text.toUcs4();
    if (characters.isEmpty() || characters.size() > 512 ||
        image.format() != QImage::Format_Grayscale8 || !image.rect().contains(word))
        fail("縦書きの語の位置を確認できません。結果は反映しません。");
    // The pinned recognizer returns invalid symbol rectangles for vertical LSTM
    // words. Its column rectangles are valid. Locate separating white rows in that
    // rectangle, then bound the actual ink of each recognized character.
    QVector<QPair<int, int>> gaps;
    int gap = -1;
    for (int y = word.top(); y <= word.bottom(); ++y)
    {
        const auto pixels = image.constScanLine(y);
        bool ink = false;
        for (int x = word.left(); x <= word.right() && !ink; ++x)
            ink = pixels[x] < 128;
        if (!ink && gap < 0)
            gap = y;
        if (ink && gap >= 0)
        {
            if (gap > word.top())
                gaps << qMakePair(gap, y);
            gap = -1;
        }
    }
    std::stable_sort(gaps.begin(), gaps.end(),
                     [](auto a, auto b) { return a.second - a.first > b.second - b.first; });
    if (gaps.size() < characters.size() - 1)
        fail("縦書き文字を分ける空きが不足しています。結果は反映しません。");
    QVector<int> boundaries{word.top()};
    for (int i = 0; i < characters.size() - 1; ++i)
        boundaries << (gaps[i].first + gaps[i].second) / 2;
    boundaries << word.bottom() + 1;
    std::sort(boundaries.begin(), boundaries.end());
    QVector<Symbol> result;
    for (int i = 0; i < characters.size(); ++i)
    {
        QRect ink;
        for (int y = boundaries[i]; y < boundaries[i + 1]; ++y)
        {
            const auto pixels = image.constScanLine(y);
            for (int x = word.left(); x <= word.right(); ++x)
                if (pixels[x] < 128)
                    ink = ink.isValid() ? ink.united(QRect(x, y, 1, 1)) : QRect(x, y, 1, 1);
        }
        if (!ink.isValid())
            fail("縦書きの字形の位置を確認できません。結果は反映しません。");
        result << Symbol{QString::fromUcs4(&characters[i], 1),
                         QRectF(ink.x() * 72.0 / 300, ink.y() * 72.0 / 300,
                                ink.width() * 72.0 / 300, ink.height() * 72.0 / 300)};
    }
    return result;
}
} // namespace
PDFDocument verticalOcrLayer(tesseract::TessBaseAPI& api, QSizeF points, const QImage& image,
                             bool columnRuns)
{
    using namespace detail;
    QVector<Symbol> symbols;
    QString allText;
    std::unique_ptr<tesseract::ResultIterator> iterator(api.GetIterator());
    if (iterator)
        do
        {
            std::unique_ptr<char[]> utf8(iterator->GetUTF8Text(tesseract::RIL_TEXTLINE));
            const auto text = QString::fromUtf8(utf8.get()).trimmed();
            if (text.trimmed().isEmpty())
                continue;
            tesseract::Orientation orientation;
            tesseract::WritingDirection direction;
            tesseract::TextlineOrder order;
            float deskew;
            iterator->Orientation(&orientation, &direction, &order, &deskew);
            if (direction != tesseract::WRITING_DIRECTION_TOP_TO_BOTTOM)
                fail("縦書きと横書きが混在する画像は、この縦書きOCRでは未対応です。");
            int left = 0, top = 0, right = 0, bottom = 0;
            if (!iterator->BoundingBox(tesseract::RIL_TEXTLINE, &left, &top, &right, &bottom) ||
                right <= left || bottom <= top)
                fail(QString("縦書き文字「%1」の位置を確認できません（%2,%3,%4,%"
                             "5）。結果は反映しません。")
                         .arg(text)
                         .arg(left)
                         .arg(top)
                         .arg(right)
                         .arg(bottom));
            if (symbols.size() >= 100000)
                fail("縦書きOCRの1ページ文字数が上限を超えています。");
            auto column = columnSymbols(text, QRect(left, top, right - left, bottom - top), image);
            column.last().columnEnd = true;
            symbols += column;
            if (symbols.size() > 100000)
                fail("縦書きOCRの1ページ文字数が上限を超えています。");
            allText += text;
        } while (iterator->Next(tesseract::RIL_TEXTLINE));
    if (symbols.isEmpty())
        fail("縦書き文字の位置を確認できません。結果は反映しません。");

    // Let Qt produce the same valid embedded subset and Unicode mapping as the
    // horizontal layer. Its temporary text stream is replaced below.
    QFont font(signatureFont());
    font.setPixelSize(100);
    font.setStyleStrategy(QFont::NoFontMerging);
    const auto raw = QRawFont::fromFont(font);
    for (auto character : allText.toUcs4())
        if (!raw.supportsCharacter(character))
            fail(QString("縦書き文字層の字体が U+%1 に対応していません。")
                     .arg(character, 4, 16, QChar('0')));
    PDFContentStreamBuilder stream(points, PDFContentStreamBuilder::CoordinateSystem::Qt);
    auto painter = stream.begin();
    painter->setFont(font);
    painter->drawText(QPointF(), allText);
    auto generated = stream.end(painter);
    auto document = correctFontUnicode(generated.document, allText, raw);
    const auto page = document.getCatalog()->getPage(0);
    const auto resources = document.getObject(page->getResources());
    const auto fonts = document.getObject(resources.getDictionary()->get("Font"));
    if (!fonts.isDictionary() || fonts.getDictionary()->getCount() != 1)
        fail("縦書きの埋込み字体を確認できません。");
    const auto reference = fonts.getDictionary()->getValue(0);
    const auto encoder = PDFFont::createFont(reference, "TatsujinVertical", &document);
    PDFDocumentBuilder builder(&document);
    auto fontDictionary = *document.getObject(reference).getDictionary();
    if (document.getObject(fontDictionary.get("Encoding")).getString() != "Identity-H")
        fail("縦書きの文字コードを確認できません。");
    const auto descendants = document.getObject(fontDictionary.get("DescendantFonts"));
    if (!descendants.isArray() || descendants.getArray()->getCount() != 1)
        fail("縦書きの埋込み字体の形が不正です。");
    const auto descendant = descendants.getArray()->getItem(0);
    auto descendantDictionary = *document.getObject(descendant).getDictionary();
    if (columnRuns)
    {
        auto descriptor =
            *document.getObject(descendantDictionary.get("FontDescriptor")).getDictionary();
        const auto program = document.getObject(descriptor.get("FontFile2"));
        if (!program.isStream())
            fail("回転した縦書きOCRの字体形式を確認できません。");
        const auto bytes = columnOcrFont(document.getDecodedStream(program.getStream()));
        PDFDictionary programInfo;
        set(programInfo, "Length1", PDFObject::createInteger(bytes.size()));
        set(descriptor, "FontFile2",
            PDFObject::createReference(builder.addObject(streamObject(programInfo, bytes))));
        set(descendantDictionary, "FontDescriptor",
            PDFObject::createReference(builder.addObject(dictObject(descriptor))));
    }
    const auto originalMap = document.getObject(descendantDictionary.get("CIDToGIDMap"));
    QByteArray glyphMap;
    if (originalMap.isStream())
        glyphMap = document.getDecodedStream(originalMap.getStream());
    else if (!originalMap.isName() || originalMap.getString() != "Identity")
        fail("縦書きの埋込み字形対応を確認できません。");
    struct Placement
    {
        double sx, sy, x, y, originY;
        quint16 originalCid;
    };
    QVector<Placement> positions;
    const QFontMetricsF metrics(font);
    for (const auto& symbol : symbols)
    {
        const auto encoded = encoder->encodeText(symbol.text);
        const auto bounds = metrics.tightBoundingRect(symbol.text);
        if (!encoded.isValid || encoded.encodedText.size() != 2 || bounds.isEmpty())
            fail("縦書きの字形を1文字の位置へ配置できません。結果は反映しません。");
        const double sx = symbol.box.width() / bounds.width();
        const double sy = symbol.box.height() / bounds.height();
        const double x = symbol.box.left() - sx * bounds.left();
        const double y = points.height() - symbol.box.top();
        const auto cid = (quint16(quint8(encoded.encodedText[0])) << 8) |
                         quint16(quint8(encoded.encodedText[1]));
        positions << Placement{sx, sy, x, y, -bounds.top() * 10, quint16(cid)};
    }
    QByteArray commands;
    auto pageDictionary = *builder.getObjectByReference(page->getPageReference()).getDictionary();
    PDFDictionary layerFonts, layerResources;
    // A character's measured baseline pitch can differ at each occurrence. Use
    // occurrence CIDs sharing the same embedded font program, rather than one
    // average W2 per Unicode character. Readers then need not invent spaces.
    // Split at the two-byte CID limit; the page's existing 100000-symbol cap stays.
    for (int start = 0; start < symbols.size(); start += 65535)
    {
        const int count = qMin(65535, int(symbols.size()) - start);
        const QByteArray resource = "TatsujinVertical" + QByteArray::number(start / 65535);
        QByteArray mapping(2, 0),
            unicode =
                "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n"
                "/CMapName /TatsujinVerticalUnicode def\n/CMapType 2 def\n"
                "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        QByteArray chunk;
        std::vector<PDFObject> widths, verticalWidths;
        int columnStart = start;
        for (int index = 0; index < count; ++index)
        {
            const int i = start + index;
            const auto& position = positions[i];
            const auto code = QByteArray::number(index + 1, 16).rightJustified(4, '0');
            if (glyphMap.isEmpty())
            {
                mapping += char(position.originalCid >> 8);
                mapping += char(position.originalCid & 255);
            }
            else
            {
                if (2 * position.originalCid + 2 > glyphMap.size())
                    fail("縦書き字形の対応が不完全です。結果は反映しません。");
                mapping += glyphMap.mid(2 * position.originalCid, 2);
            }
            QByteArray utf16;
            for (const auto character : symbols[i].text)
            {
                utf16 += char(character.unicode() >> 8);
                utf16 += char(character.unicode() & 255);
            }
            chunk += '<' + code + "> <" + utf16.toHex() + ">\n";
            if ((index + 1) % 100 == 0 || index + 1 == count)
            {
                unicode +=
                    QByteArray::number(index % 100 + 1) + " beginbfchar\n" + chunk + "endbfchar\n";
                chunk.clear();
            }
            const bool runStart = index == 0 || symbols[i - 1].columnEnd;
            const bool runEnd = symbols[i].columnEnd || index + 1 == count;
            if (runStart)
                columnStart = i;
            const auto& column = positions[columnStart];
            const double sy = columnRuns ? column.sy : position.sy;
            // Keep one PDF text object per column; compensate a quarter-em of
            // vertical tracking with TJ. The 2em W and hmtx remain consistent.
            const double advance =
                (columnRuns ? runEnd : symbols[i].columnEnd)
                    ? -1000
                    : (positions[i + 1].y - position.y) * 10 / sy - (columnRuns ? 250 : 0);
            widths.push_back(
                number(columnRuns ? 2000 : pdfGlyphAdvance(encoder, position.originalCid, {}, {})));
            if (!std::isfinite(advance) || advance >= 0)
                fail("縦書き文字の送りを確認できません。結果は反映しません。");
            verticalWidths.insert(
                verticalWidths.end(),
                {number(advance), number(columnRuns ? (column.x - position.x) * 10 / column.sx : 0),
                 number(columnRuns ? position.originY * position.sy / sy : position.originY)});
            if (columnRuns)
            {
                const auto bounds = metrics.tightBoundingRect(symbols[i].text);
                if (qMax(qAbs((column.sx - position.sx) * bounds.width()),
                         qAbs((column.sy - position.sy) * bounds.height())) *
                        25.4 / 72 >
                    .5)
                    fail("回転した縦書きの列で文字の大きさが混在しています。OCRを反映しません。");
                if (runStart)
                    commands += "/Span BMC\nq BT /" + resource + " 100 Tf 3 Tr " +
                                QString("%1 0 0 %2 %3 %4 Tm [")
                                    .arg(decimal(column.sx), decimal(column.sy), decimal(column.x),
                                         decimal(column.y))
                                    .toLatin1();
                commands += '<' + code + "> ";
                commands += runEnd ? "] TJ ET Q\nEMC\n" : "-250 ";
            }
            else
                commands +=
                    "/Span BMC\nq BT /" + resource + " 100 Tf 3 Tr " +
                    QString("%1 0 0 %2 %3 %4 Tm <%5> Tj ET Q\nEMC\n")
                        .arg(decimal(position.sx), decimal(position.sy), decimal(position.x),
                             decimal(position.y), QString::fromLatin1(code))
                        .toLatin1();
        }
        unicode += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
        auto descendantCopy = descendantDictionary;
        if (columnRuns)
            set(descendantCopy, "DW", number(2000));
        set(descendantCopy, "W",
            arrObject({PDFObject::createInteger(1), arrObject(std::move(widths))}));
        set(descendantCopy, "DW2", arrObject({number(0), number(-1000)}));
        set(descendantCopy, "W2",
            arrObject({PDFObject::createInteger(1), arrObject(std::move(verticalWidths))}));
        set(descendantCopy, "CIDToGIDMap",
            PDFObject::createReference(builder.addObject(streamObject({}, mapping))));
        auto face = fontDictionary;
        set(face, "Encoding", PDFObject::createName("Identity-V"));
        set(face, "DescendantFonts",
            arrObject({PDFObject::createReference(builder.addObject(dictObject(descendantCopy)))}));
        set(face, "ToUnicode",
            PDFObject::createReference(builder.addObject(streamObject({}, unicode))));
        layerFonts.setEntry(PDFInplaceOrMemoryString(resource),
                            PDFObject::createReference(builder.addObject(dictObject(face))));
    }
    set(layerResources, "Font", dictObject(layerFonts));
    set(pageDictionary, "Resources", dictObject(layerResources));
    set(pageDictionary, "Contents",
        PDFObject::createReference(builder.addObject(streamObject({}, commands))));
    builder.setObject(page->getPageReference(), dictObject(pageDictionary));
    return builder.build();
}
} // namespace tatsu
