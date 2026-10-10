#include "vertical_ocr_layer.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include <algorithm>
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
PDFDocument verticalOcrLayer(tesseract::TessBaseAPI& api, QSizeF points, const QImage& image)
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
            symbols += columnSymbols(text, QRect(left, top, right - left, bottom - top), image);
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
    set(fontDictionary, "Encoding", PDFObject::createName("Identity-V"));
    const auto descendants = document.getObject(fontDictionary.get("DescendantFonts"));
    if (!descendants.isArray() || descendants.getArray()->getCount() != 1)
        fail("縦書きの埋込み字体の形が不正です。");
    const auto descendant = descendants.getArray()->getItem(0);
    auto descendantDictionary = *document.getObject(descendant).getDictionary();
    // Every symbol has its own absolute matrix. Zero vertical origins keep that
    // matrix at the glyph's original baseline; Identity-V describes reading order.
    set(descendantDictionary, "DW2", arrObject({number(0), number(-1000)}));
    set(descendantDictionary, "W2",
        arrObject({PDFObject::createInteger(0), PDFObject::createInteger(65535), number(-1000),
                   number(0), number(0)}));
    builder.setObject(descendant.getReference(), dictObject(descendantDictionary));
    builder.setObject(reference.getReference(), dictObject(fontDictionary));

    QByteArray commands;
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
        const double y = points.height() - symbol.box.bottom() + sy * bounds.bottom();
        commands += QString("q BT /TatsujinVertical 100 Tf 3 Tr %1 0 0 %2 %3 %4 Tm "
                            "<%5> Tj ET Q\n")
                        .arg(decimal(sx), decimal(sy), decimal(x), decimal(y),
                             QString::fromLatin1(encoded.encodedText.toHex()))
                        .toLatin1();
    }
    auto pageDictionary = *builder.getObjectByReference(page->getPageReference()).getDictionary();
    PDFDictionary layerFonts, layerResources;
    set(layerFonts, "TatsujinVertical", reference);
    set(layerResources, "Font", dictObject(layerFonts));
    set(pageDictionary, "Resources", dictObject(layerResources));
    set(pageDictionary, "Contents",
        PDFObject::createReference(builder.addObject(streamObject({}, commands))));
    builder.setObject(page->getPageReference(), dictObject(pageDictionary));
    return builder.build();
}
} // namespace tatsu
