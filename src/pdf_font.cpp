#include "document.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"

namespace tatsu
{
using namespace pdf;
using namespace detail;
QString asset(const QString& relative)
{
    const QString root =
        qEnvironmentVariable("TATSU_ASSETS", QCoreApplication::applicationDirPath() + "/assets");
    return root + "/" + relative;
}
QString signatureFont()
{
    static QString family;
    if (family.isEmpty())
    {
        int id = QFontDatabase::addApplicationFont(asset("fonts/NotoSansJP.ttf"));
        if (id < 0)
            fail("同梱の日本語フォントを読み込めません。");
        family = QFontDatabase::applicationFontFamilies(id).value(0);
    }
    return family;
}
PDFDocument correctFontUnicode(const PDFDocument& doc, const QString& text, const QRawFont& font)
{
    // Qt's subset writer can choose a different Unicode alias for a shared glyph
    // (e.g. a Kangxi radical instead of the recognized Japanese character).
    // Resolve aliases against the actual input and this exact font, not a general
    // Unicode normalization that would change unrelated text or test expectations.
    QHash<quint32, QString> used;
    QSet<quint32> ambiguous;
    QSet<QString> actual;
    for (auto cp : text.toUcs4())
    {
        QString c = QString::fromUcs4(&cp, 1);
        actual.insert(c);
        auto glyph = font.glyphIndexesForString(c).value(0);
        if (!glyph)
            continue;
        if (used.contains(glyph) && used[glyph] != c)
            ambiguous.insert(glyph);
        else
            used[glyph] = c;
    }
    PDFDocumentBuilder builder(&doc);
    auto resources = doc.getObject(doc.getCatalog()->getPage(0)->getResources());
    auto fonts = doc.getObject(resources.getDictionary()->get("Font"));
    if (!fonts.isDictionary())
        return doc;
    for (size_t i = 0; i < fonts.getDictionary()->getCount(); ++i)
    {
        auto f = doc.getObject(fonts.getDictionary()->getValue(i));
        if (!f.isDictionary())
            continue;
        auto ref = f.getDictionary()->get("ToUnicode");
        auto cmap = doc.getObject(ref);
        if (!ref.isReference() || !cmap.isStream())
            continue;
        QString source = QString::fromLatin1(doc.getDecodedStream(cmap.getStream()));
        QRegularExpression arrays("\\[([^\\]]*)\\]");
        auto matches = arrays.globalMatch(source);
        QVector<QPair<int, QPair<int, QString>>> replacements;
        while (matches.hasNext())
        {
            auto match = matches.next();
            QString values = match.captured(1);
            QRegularExpression hex("<([0-9A-Fa-f]{4})>");
            auto codes = hex.globalMatch(values);
            QVector<QPair<int, QString>> edits;
            while (codes.hasNext())
            {
                auto code = codes.next();
                QString alias(QChar(code.captured(1).toUShort(nullptr, 16)));
                auto glyph = font.glyphIndexesForString(alias).value(0);
                if (ambiguous.contains(glyph))
                    fail("同一字形に複数の文字コードがあるため、文字情報を安全に保存できません。");
                if (!glyph || actual.contains(alias) || !used.contains(glyph) ||
                    used[glyph].size() != 1)
                    continue;
                edits.append({int(code.capturedStart(1)),
                              QString("%1")
                                  .arg(uint(used[glyph][0].unicode()), 4, 16, QChar('0'))
                                  .toUpper()});
            }
            for (auto it = edits.crbegin(); it != edits.crend(); ++it)
                values.replace(it->first, 4, it->second);
            replacements.append(
                {int(match.capturedStart(1)), {int(match.capturedLength(1)), values}});
        }
        for (auto it = replacements.crbegin(); it != replacements.crend(); ++it)
            source.replace(it->first, it->second.first, it->second.second);
        builder.setObject(ref.getReference(), streamObject({}, source.toLatin1()));
    }
    return builder.build();
}
} // namespace tatsu
