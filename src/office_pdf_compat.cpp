#include "office_pdf_compat.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfparser.h"
#include <set>

namespace tatsu
{
namespace
{
struct Replacement
{
    qsizetype begin, end;
    QByteArray bytes;
};
} // namespace
QByteArray splitOfficeClosingAdjustments(const QByteArray& content)
{
    // Positive TJ numbers close a gap. Some extractors mistake their magnitude
    // for a word space. Splitting an integer retains the exact summed advance,
    // glyph codes and genuine (negative) word spacing; no text is substituted.
    pdf::PDFLexicalAnalyzer lexer(content.constBegin(), content.constEnd());
    QVector<Replacement> replacements, pending;
    bool inArray = false, endedArray = false;
    while (!lexer.isAtEnd())
    {
        lexer.skipWhitespaceAndComments();
        const auto begin = lexer.pos();
        const auto token = lexer.fetch();
        using Type = pdf::PDFLexicalAnalyzer::TokenType;
        if (token.type == Type::Command && token.data.toByteArray() == "BI")
            return content; // Inline image bytes are not lexical PDF operators.
        if (endedArray)
        {
            if (token.type == Type::Command && token.data.toByteArray() == "TJ")
                replacements += pending;
            pending.clear();
            endedArray = false;
        }
        if (token.type == Type::ArrayStart)
        {
            if (inArray)
                return content;
            inArray = true;
            pending.clear();
        }
        else if (token.type == Type::ArrayEnd)
        {
            endedArray = inArray;
            inArray = false;
        }
        else if (inArray && token.type == Type::Integer)
        {
            const auto value = token.data.toLongLong();
            if (value > 25 && value <= 10000)
            {
                QByteArray bytes;
                for (qint64 remaining = value; remaining > 0; remaining -= 25)
                {
                    if (!bytes.isEmpty())
                        bytes += ' ';
                    bytes += QByteArray::number(qMin(qint64(25), remaining));
                }
                pending << Replacement{begin, lexer.pos(), bytes};
            }
        }
        else if (inArray && token.type != Type::String && token.type != Type::Real)
            return content;
        if (replacements.size() + pending.size() > 100000)
            fail("Office PDFの文字調整が上限を超えています。");
    }
    QByteArray result;
    qsizetype copied = 0;
    for (const auto& replacement : replacements)
    {
        result += content.mid(copied, replacement.begin - copied);
        result += replacement.bytes;
        copied = replacement.end;
        if (result.size() > 64 * 1024 * 1024)
            fail("Office PDFのページ内容が上限を超えています。");
    }
    result += content.mid(copied);
    return result;
}
PDFDocument normalizeOfficeClosingAdjustments(const PDFDocument& document,
                                              const std::function<bool()>& cancelled)
{
    using namespace detail;
    std::set<PDFObjectReference> contents;
    const auto collect = [&](PDFObject object)
    {
        if (object.isReference())
        {
            const auto resolved = document.getObject(object);
            if (resolved.isStream())
                contents.insert(object.getReference());
            else if (resolved.isArray())
                for (const auto& item : *resolved.getArray())
                    if (item.isReference())
                        contents.insert(item.getReference());
        }
        else if (object.isArray())
            for (const auto& item : *object.getArray())
                if (item.isReference())
                    contents.insert(item.getReference());
    };
    for (size_t i = 0; i < document.getCatalog()->getPageCount(); ++i)
    {
        const auto page =
            document.getObjectByReference(document.getCatalog()->getPage(i)->getPageReference());
        if (!page.isDictionary())
            fail("Office PDFのページ辞書を確認できません。");
        collect(page.getDictionary()->get("Contents"));
    }
    const auto& objects = document.getStorage().getObjects();
    for (size_t i = 0; i < objects.size(); ++i)
    {
        const auto& entry = objects[i];
        if (entry.object.isStream() &&
            document.getObject(entry.object.getStream()->getDictionary()->get("Subtype")) ==
                PDFObject::createName("Form"))
            contents.insert(PDFObjectReference(i, entry.generation));
    }
    PDFDocumentBuilder builder(&document);
    bool changed = false;
    qint64 total = 0;
    for (const auto& reference : contents)
    {
        if (cancelled && cancelled())
            fail("変換を取り消しました。");
        const auto object = document.getObjectByReference(reference);
        if (!object.isStream())
            fail("Office PDFのページ内容を確認できません。");
        const auto bytes = document.getDecodedStream(object.getStream());
        total += bytes.size();
        if (bytes.size() > 64 * 1024 * 1024 || total > 256 * 1024 * 1024)
            fail("Office PDFの展開内容が上限を超えています。");
        const auto normalized = splitOfficeClosingAdjustments(bytes);
        if (normalized == bytes)
            continue;
        auto attributes = *object.getStream()->getDictionary();
        attributes.removeEntry("Filter");
        attributes.removeEntry("DecodeParms");
        builder.setObject(reference, streamObject(attributes, normalized));
        changed = true;
    }
    return changed ? builder.build() : document;
}
} // namespace tatsu
