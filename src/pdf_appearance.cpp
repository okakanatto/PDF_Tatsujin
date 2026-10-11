#include "pdf_appearance.h"
#include "pdf_objects.h"
#include "pdfpainter.h"

namespace tatsu
{
using namespace detail;
PDFObjectReference painterAppearance(PDFDocumentBuilder& builder, QSizeF dimensions,
                                     const std::function<void(QPainter*)>& draw,
                                     const QString& text, const QRawFont& font)
{
    if (!text.isEmpty())
        for (auto character : text.toUcs4())
            if (character != 10 && character != 13 && !font.supportsCharacter(character))
                fail(QString("対応していない文字があります: U+%1")
                         .arg(character, 4, 16, QChar('0')));
    PDFContentStreamBuilder stream(dimensions, PDFContentStreamBuilder::CoordinateSystem::Qt);
    auto painter = stream.begin();
    draw(painter);
    auto appearance = stream.end(painter);
    if (!text.isEmpty())
        appearance.document = correctFontUnicode(appearance.document, text, font);
    const auto resources =
        builder.copyFrom({appearance.resources}, appearance.document.getStorage(), true).at(0);
    PDFDocumentBuilder source(&appearance.document);
    const auto contents = appearance.document.getObject(appearance.contents);
    QByteArray commands;
    if (contents.isArray())
        for (const auto& part : *contents.getArray())
            commands +=
                source.getDecodedStream(appearance.document.getObject(part).getStream()) + '\n';
    else
        commands = source.getDecodedStream(contents.getStream());
    PDFDictionary form;
    set(form, "Type", PDFObject::createName("XObject"));
    set(form, "Subtype", PDFObject::createName("Form"));
    set(form, "BBox", rectObject(QRectF(QPointF(), dimensions)));
    set(form, "Resources", resources);
    return builder.addObject(streamObject(form, commands));
}
} // namespace tatsu
