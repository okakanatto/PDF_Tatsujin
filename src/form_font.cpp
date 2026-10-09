#include "form_font.h"
#include "pdf_objects.h"
#include <cmath>

namespace tatsu
{
using namespace detail;
namespace
{
const QByteArray fontHash = "276f192d1ed547c1183b1fc8ee2f906d4d1ba66e74897434f6947f670aa1286e";
const QByteArray mapHash = "ca12d290cbd5488a08d7a7ce7e877008ead2c4322144315d3f457f6e7b4ee62b";
struct Character
{
    quint16 cid, gid;
    double width;
};
struct FontData
{
    QByteArray font;
    QJsonObject metrics;
    QMap<char32_t, Character> characters;
};
QByteArray read(const QString& name, qsizetype maximum, const QByteArray& hash)
{
    QFile file(asset("fonts/" + name));
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximum)
        fail("フォーム用の日本語フォントを読み込めません。");
    auto data = file.readAll();
    if (QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != hash)
        fail("フォーム用の日本語フォントが変更されています。");
    return data;
}
const FontData& data()
{
    static const FontData value = []
    {
        FontData result;
        result.font = read("TatsujinSansJP-Regular.ttf", 8 * 1024 * 1024, fontHash);
        result.metrics =
            QJsonDocument::fromJson(read("TatsujinSansJP-Regular.json", 1024 * 1024, mapHash))
                .object();
        if (result.metrics["version"].toInt() != 1 ||
            result.metrics["font_sha256"].toString().toLatin1() != fontHash)
            fail("フォーム用のフォント対応表が不正です。");
        quint16 cid = 1;
        for (const auto& row : result.metrics["characters"].toArray())
        {
            const auto entry = row.toArray();
            result.characters.insert(char32_t(entry[0].toInt()),
                                     {cid++, quint16(entry[1].toInt()), entry[2].toDouble()});
        }
        return result;
    }();
    return value;
}
PDFObjectReference compressed(PDFDocumentBuilder& builder, PDFDictionary dictionary,
                              const QByteArray& bytes)
{
    set(dictionary, "Filter", PDFObject::createName("FlateDecode"));
    return builder.addObject(streamObject(dictionary, qCompress(bytes, 9).mid(4)));
}
QByteArray n(double value)
{
    return QByteArray::number(value, 'f', 8);
}
QByteArray encoded(const QString& text)
{
    QByteArray result;
    for (auto cp : text.toUcs4())
    {
        const auto found = data().characters.constFind(cp);
        if (found == data().characters.cend())
            fail(QString("フォーム用フォントに対応していない文字です: U+%1")
                     .arg(uint(cp), 4, 16, QChar('0')));
        result += QByteArray::number(found->cid, 16).rightJustified(4, '0');
    }
    return result;
}
double textWidth(const QString& text, double size)
{
    double result = 0;
    for (auto cp : text.toUcs4())
    {
        const auto found = data().characters.constFind(cp);
        if (found == data().characters.cend())
            fail(QString("フォーム用フォントに対応していない文字です: U+%1")
                     .arg(uint(cp), 4, 16, QChar('0')));
        result += found->width * size / 1000;
    }
    return result;
}
QStringList lines(const QString& text, bool wrap, double width, double size)
{
    QStringList result;
    QString line;
    double current = 0;
    for (auto cp : text.toUcs4())
    {
        if (cp == '\r')
            continue;
        if (cp == '\n')
        {
            result << line;
            line.clear();
            current = 0;
            continue;
        }
        const auto character = QString::fromUcs4(&cp, 1);
        const double advance = textWidth(character, size);
        if (wrap && !line.isEmpty() && current + advance > width)
        {
            result << line;
            line.clear();
            current = 0;
        }
        line += character;
        current += advance;
    }
    result << line;
    return result;
}
} // namespace
void validateFormFontText(const QString& text)
{
    for (auto cp : text.toUcs4())
        if (cp != '\r' && cp != '\n' && !data().characters.contains(cp))
            fail(QString("フォーム用フォントに対応していない文字です: U+%1")
                     .arg(uint(cp), 4, 16, QChar('0')));
}
PDFObjectReference embedFormFont(PDFDocumentBuilder& builder)
{
    const auto& source = data();
    PDFDictionary file;
    set(file, "Length1", PDFObject::createInteger(source.font.size()));
    const auto embedded = compressed(builder, file, source.font);
    PDFDictionary descriptor;
    set(descriptor, "Type", PDFObject::createName("FontDescriptor"));
    set(descriptor, "FontName", PDFObject::createName("TatsujinSansJP-Regular"));
    set(descriptor, "Flags", PDFObject::createInteger(4));
    std::vector<PDFObject> box;
    for (const auto& value : source.metrics["bbox"].toArray())
        box.push_back(number(value.toDouble()));
    set(descriptor, "FontBBox", arrObject(std::move(box)));
    for (auto key : {"ascent", "descent", "cap_height"})
        set(descriptor,
            QByteArray(key) == "ascent"    ? "Ascent"
            : QByteArray(key) == "descent" ? "Descent"
                                           : "CapHeight",
            number(source.metrics[key].toDouble()));
    set(descriptor, "ItalicAngle", number(0));
    set(descriptor, "StemV", number(80));
    set(descriptor, "FontFile2", PDFObject::createReference(embedded));
    PDFDictionary system;
    set(system, "Registry", PDFObject::createString("Adobe"));
    set(system, "Ordering", PDFObject::createString("Identity"));
    set(system, "Supplement", PDFObject::createInteger(0));
    QByteArray mapping(2, 0);
    std::vector<PDFObject> widths{number(source.metrics["notdef_width"].toDouble())};
    QByteArray unicode =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n"
        "/CMapName /TatsujinUnicode def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    QByteArray chunk;
    int count = 0;
    auto flush = [&]
    {
        if (count)
            unicode += QByteArray::number(count) + " beginbfchar\n" + chunk + "endbfchar\n";
        count = 0;
        chunk.clear();
    };
    for (auto it = source.characters.cbegin(); it != source.characters.cend(); ++it)
    {
        mapping += char(it->gid >> 8);
        mapping += char(it->gid & 255);
        widths.push_back(number(it->width));
        const char32_t cp = it.key();
        const auto text = QString::fromUcs4(&cp, 1);
        QByteArray utf16;
        for (auto c : text)
        {
            utf16 += char(c.unicode() >> 8);
            utf16 += char(c.unicode() & 255);
        }
        chunk += '<' + QByteArray::number(it->cid, 16).rightJustified(4, '0') + "> <" +
                 utf16.toHex() + ">\n";
        if (++count == 100)
            flush();
    }
    flush();
    unicode += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    PDFDictionary cid;
    set(cid, "Type", PDFObject::createName("Font"));
    set(cid, "Subtype", PDFObject::createName("CIDFontType2"));
    set(cid, "BaseFont", PDFObject::createName("TatsujinSansJP-Regular"));
    set(cid, "CIDSystemInfo", dictObject(system));
    set(cid, "FontDescriptor",
        PDFObject::createReference(builder.addObject(dictObject(descriptor))));
    set(cid, "DW", number(1000));
    set(cid, "W", arrObject({number(0), arrObject(std::move(widths))}));
    set(cid, "CIDToGIDMap", PDFObject::createReference(compressed(builder, {}, mapping)));
    PDFDictionary font;
    set(font, "Type", PDFObject::createName("Font"));
    set(font, "Subtype", PDFObject::createName("Type0"));
    set(font, "BaseFont", PDFObject::createName("TatsujinSansJP-Regular"));
    set(font, "Encoding", PDFObject::createName("Identity-H"));
    set(font, "DescendantFonts",
        arrObject({PDFObject::createReference(builder.addObject(dictObject(cid)))}));
    set(font, "ToUnicode", PDFObject::createReference(compressed(builder, {}, unicode)));
    set(font, "TatsujinFontSHA256", PDFObject::createString(fontHash));
    return builder.addObject(dictObject(font));
}
bool isFormFont(const PDFDocument& document, PDFObjectReference reference)
{
    const auto font = document.getObjectByReference(reference);
    if (!font.isDictionary() || !font.getDictionary()->get("TatsujinFontSHA256").isString() ||
        font.getDictionary()->get("TatsujinFontSHA256").getString() != fontHash)
        return false;
    const auto descendants = document.getObject(font.getDictionary()->get("DescendantFonts"));
    if (!descendants.isArray() || descendants.getArray()->getCount() != 1)
        return false;
    const auto cid = document.getObject(descendants.getArray()->getItem(0));
    if (!cid.isDictionary())
        return false;
    const auto descriptor = document.getObject(cid.getDictionary()->get("FontDescriptor"));
    if (!descriptor.isDictionary())
        return false;
    const auto file = document.getObject(descriptor.getDictionary()->get("FontFile2"));
    return file.isStream() && QCryptographicHash::hash(document.getDecodedStream(file.getStream()),
                                                       QCryptographicHash::Sha256)
                                      .toHex() == fontHash;
}
PDFObjectReference formFontAppearance(PDFDocumentBuilder& builder, PDFObjectReference font,
                                      QSizeF dimensions, const FormField& field,
                                      const QStringList& values, double unit, int rotation)
{
    if (!std::isfinite(unit) || unit <= 0 || dimensions.width() <= 0 || dimensions.height() <= 0 ||
        (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270))
        fail("フォームの表示範囲が不正です。");
    const double w = dimensions.width(), h = dimensions.height(), padding = 2 / unit;
    QByteArray commands = "q 1 g 0 0 " + n(w) + ' ' + n(h) + " re f 0.4 G " + n(.8 / unit) + " w " +
                          n(.4 / unit) + ' ' + n(.4 / unit) + ' ' + n(w - .8 / unit) + ' ' +
                          n(h - .8 / unit) + " re S\n";
    commands += n(padding) + ' ' + n(padding) + ' ' + n(w - 2 * padding) + ' ' +
                n(h - 2 * padding) + " re W n\n";
    QString text = values.value(0);
    if (field.kind == FormKind::Combo)
    {
        const int index = field.exports.indexOf(text);
        if (index >= 0)
            text = field.labels[index];
    }
    double size = 12 / unit;
    QStringList rows;
    const double metrics =
        (data().metrics["ascent"].toDouble() - data().metrics["descent"].toDouble()) / 1000;
    if (field.kind != FormKind::List)
        for (;;)
        {
            rows = lines(text, field.kind == FormKind::Multiline, w - 2 * padding, size);
            bool fits = rows.size() * metrics * size <= h - 2 * padding;
            for (const auto& row : rows)
                fits = fits && textWidth(row, size) <= w - 2 * padding;
            if (fits || text.isEmpty())
                break;
            size *= .92;
            if (size < 6 / unit)
                fail("入力がフォーム欄に収まりません。内容か欄の大きさを調整してください。");
        }
    else
        rows = field.labels;
    const double leading = metrics * size;
    double baseline =
        field.kind == FormKind::Text || field.kind == FormKind::Combo
            ? (h - metrics * size) / 2 - data().metrics["descent"].toDouble() * size / 1000
            : h - padding - data().metrics["ascent"].toDouble() * size / 1000;
    int start = 0;
    if (field.kind == FormKind::List && !values.isEmpty())
    {
        start = field.labels.size();
        for (const auto& value : values)
            start = qMin(start, qMax(0, field.exports.indexOf(value)));
    }
    for (int i = start; i < rows.size(); ++i)
    {
        if (field.kind == FormKind::List &&
            baseline + data().metrics["descent"].toDouble() * size / 1000 < padding)
            break;
        const bool selected =
            field.kind == FormKind::List && values.contains(field.exports.value(i));
        if (selected)
            commands += "0.094 0.373 0.765 rg " + n(padding) + ' ' +
                        n(baseline + data().metrics["descent"].toDouble() * size / 1000) + ' ' +
                        n(w - 2 * padding) + ' ' + n(leading) + " re f\n";
        commands += "BT /TatsuJP " + n(size) + " Tf " + (selected ? "1 g " : "0 g ") + n(padding) +
                    ' ' + n(baseline) + " Td <" + encoded(rows[i]) + "> Tj ET\n";
        baseline -= leading;
    }
    commands += "Q\n";
    PDFDictionary fonts, resources, appearance;
    set(fonts, "TatsuJP", PDFObject::createReference(font));
    set(resources, "Font", dictObject(fonts));
    set(appearance, "Type", PDFObject::createName("XObject"));
    set(appearance, "Subtype", PDFObject::createName("Form"));
    set(appearance, "BBox", rectObject(QRectF(QPointF(), dimensions)));
    set(appearance, "Resources", dictObject(resources));
    if (rotation)
    {
        QTransform matrix;
        matrix.rotate(rotation);
        const auto box = matrix.mapRect(QRectF(QPointF(), dimensions));
        matrix =
            QTransform(matrix.m11(), matrix.m12(), matrix.m21(), matrix.m22(), -box.x(), -box.y());
        set(appearance, "Matrix",
            arrObject({number(matrix.m11()), number(matrix.m12()), number(matrix.m21()),
                       number(matrix.m22()), number(matrix.dx()), number(matrix.dy())}));
    }
    return compressed(builder, appearance, commands);
}
} // namespace tatsu
