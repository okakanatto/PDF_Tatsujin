#include "body_text_font.h"
#include "pdffont.h"
#include "pdfpainter.h"
#include "text_font.h"

namespace tatsu
{
EmbeddedBodyFont embedBodyTextFont(PDFDocumentBuilder& builder, const QString& family,
                                   const QString& text, const std::function<bool()>& cancelled)
{
    auto stop = [&]
    {
        if (cancelled && cancelled())
            fail("本文の字体生成を中止しました。");
    };
    stop();
    if (text.isEmpty() || text.size() > 4096 || text.contains('\n') || text.contains('\r'))
        fail("字体を生成する本文は1行、4096文字以内で指定してください。");
    const auto font = textFont(family);
    const auto raw = QRawFont::fromFont(font);
    for (auto character : text.toUcs4())
        if (!raw.supportsCharacter(character))
            fail(QString("指定した字体に対応していない文字です: U+%1")
                     .arg(character, 4, 16, QChar('0')));
    const QFontMetricsF metrics(font);
    PDFContentStreamBuilder stream(
        QSizeF(qMax(100.0, metrics.horizontalAdvance(text) + 20), metrics.lineSpacing() + 20),
        PDFContentStreamBuilder::CoordinateSystem::Qt);
    auto painter = stream.begin();
    painter->setFont(font);
    painter->setPen(Qt::black);
    painter->drawText(QPointF(0, metrics.ascent()), text);
    auto appearance = stream.end(painter);
    stop();
    appearance.document = correctFontUnicode(appearance.document, text, raw);
    const auto resources = appearance.document.getObject(appearance.resources);
    const auto fonts = appearance.document.getObject(resources.getDictionary()->get("Font"));
    if (!fonts.isDictionary() || fonts.getDictionary()->getCount() != 1)
        fail("生成した本文の字体を確認できません。別の字体を選んでください。");
    const auto reference = fonts.getDictionary()->getValue(0);
    const auto imported = PDFFont::createFont(reference, "TatsujinBodyFont", &appearance.document);
    const auto encoded = imported->encodeText(text);
    if (!encoded.isValid || encoded.encodedText.isEmpty())
        fail("生成した字体の文字コードを確認できません。");
    stop();
    const auto copied = builder.copyFrom({reference}, appearance.document.getStorage(), true).at(0);
    return {copied, encoded.encodedText};
}
} // namespace tatsu
