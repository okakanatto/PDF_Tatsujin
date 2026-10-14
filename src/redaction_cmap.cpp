#include "redaction_cmap.h"
#include "pdfparser.h"
#include <QtEndian>
#include <algorithm>

namespace tatsu
{
namespace
{
using namespace pdf;
using Type = PDFLexicalAnalyzer::TokenType;
using Token = PDFLexicalAnalyzer::Token;
using SourceCode = QPair<quint8, quint16>;

[[noreturn]] void invalid()
{
    fail("このCMapの文字コードを安全に検証できません。墨消し候補は保存していません。");
}
bool isToken(const Token& token, Type type, const QByteArray& text)
{
    return token.type == type && token.data.toByteArray() == text;
}
SourceCode sourceCode(const Token& token)
{
    if (token.type != Type::String)
        invalid();
    const auto bytes = token.data.toByteArray();
    if (bytes.size() == 1)
        return {1, quint8(bytes[0])};
    if (bytes.size() == 2)
        return {2, qFromBigEndian<quint16>(bytes.constData())};
    invalid();
}
quint16 unicodeValue(const Token& token)
{
    const auto source = sourceCode(token);
    if (source.first != 2 || QChar(source.second).isSurrogate())
        invalid();
    return source.second;
}
struct CodeSpace
{
    SourceCode from, to;
    int firstByte() const
    {
        return from.first == 1 ? from.second : from.second >> 8;
    }
    int lastByte() const
    {
        return to.first == 1 ? to.second : to.second >> 8;
    }
    bool contains(SourceCode code) const
    {
        if (code.first != from.first)
            return false;
        if (code.first == 1)
            return code.second >= from.second && code.second <= to.second;
        return (code.second >> 8) >= firstByte() && (code.second >> 8) <= lastByte() &&
               (code.second & 255) >= (from.second & 255) &&
               (code.second & 255) <= (to.second & 255);
    }
    bool conflicts(const CodeSpace& other) const
    {
        if (qMax(firstByte(), other.firstByte()) > qMin(lastByte(), other.lastByte()))
            return false;
        // Shared prefixes across lengths have no unique segmentation.
        return from.first != other.from.first || from.first == 1 ||
               qMax(from.second & 255, other.from.second & 255) <=
                   qMin(to.second & 255, other.to.second & 255);
    }
};
struct Map
{
    QVector<CodeSpace> spaces;
    QMap<SourceCode, quint16> values;
    bool contains(SourceCode code) const
    {
        return std::any_of(spaces.cbegin(), spaces.cend(),
                           [&](const CodeSpace& space) { return space.contains(code); });
    }
};
void validateSystemInfo(const PDFDocument& document, const PDFObject& reference)
{
    const auto dictionary = document.getDictionaryFromObject(reference);
    if (!dictionary || dictionary->getCount() != 3)
        invalid();
    for (size_t index = 0; index < dictionary->getCount(); ++index)
    {
        const auto key = dictionary->getKey(index).getString();
        const auto value = document.getObject(dictionary->getValue(index));
        if (key == "Registry" || key == "Ordering")
        {
            if (!value.isString() || value.getString().isEmpty())
                invalid();
        }
        else if (key == "Supplement")
        {
            if (!value.isInt() || value.getInteger() < 0 || value.getInteger() > 65535)
                invalid();
        }
        else
            invalid();
    }
    if (!dictionary->hasKey("Registry") || !dictionary->hasKey("Ordering") ||
        !dictionary->hasKey("Supplement"))
        invalid();
}
void validateStreamDictionary(const PDFDocument& document, const PDFDictionary& dictionary)
{
    for (size_t index = 0; index < dictionary.getCount(); ++index)
    {
        const auto key = dictionary.getKey(index).getString();
        if (key == "Length" || key == "Filter" || key == "DecodeParms")
            continue;
        const auto value = document.getObject(dictionary.getValue(index));
        if (key == "WMode")
        {
            if (value != PDFObject::createInteger(0))
                invalid();
        }
        else if (key == "Type")
        {
            if (value != PDFObject::createName("CMap"))
                invalid();
        }
        else if (key == "CMapName")
        {
            if (!value.isName() || value.getString().isEmpty() || value.getString() == "UseCMap")
                invalid();
        }
        else if (key == "CIDSystemInfo")
            validateSystemInfo(document, dictionary.getValue(index));
        else
            invalid();
    }
}
class MapReader
{
public:
    MapReader(QVector<Token> tokens, bool unicode, std::function<bool()> cancelled)
        : tokens(std::move(tokens)), unicode(unicode), cancelled(std::move(cancelled))
    {
    }
    Map read()
    {
        const bool wrapped = !tokens.isEmpty() && isToken(tokens.first(), Type::Name, "CIDInit");
        if (wrapped)
        {
            expect(Type::Name, "CIDInit");
            expect(Type::Name, "ProcSet");
            expect(Type::Command, "findresource");
            expect(Type::Command, "begin");
            const auto count = fetch();
            if (count.type != Type::Integer || count.data.toLongLong() < 1 ||
                count.data.toLongLong() > 256)
                invalid();
            expect(Type::Command, "dict");
            expect(Type::Command, "begin");
        }
        expect(Type::Command, "begincmap");
        while (true)
        {
            const auto token = fetch();
            if (isToken(token, Type::Command, "endcmap"))
                break;
            if (token.type == Type::Name)
                readMetadata(token.data.toByteArray());
            else if (token.type == Type::Integer)
                readBlock(token.data.toLongLong());
            else
                invalid();
        }
        if (wrapped)
        {
            expect(Type::Command, "CMapName");
            expect(Type::Command, "currentdict");
            expect(Type::Name, "CMap");
            expect(Type::Command, "defineresource");
            expect(Type::Command, "pop");
            expect(Type::Command, "end");
            expect(Type::Command, "end");
        }
        if (index != tokens.size() || result.spaces.isEmpty() || result.values.isEmpty())
            invalid();
        for (auto it = result.values.cbegin(); it != result.values.cend(); ++it)
            if (!result.contains(it.key()))
                invalid();
        return std::move(result);
    }

private:
    Token fetch()
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
        if (index >= tokens.size())
            invalid();
        return tokens[index++];
    }
    void expect(Type type, const QByteArray& text)
    {
        if (!isToken(fetch(), type, text))
            invalid();
    }
    void readSystemInfo()
    {
        if (fetch().type != Type::DictionaryStart)
            invalid();
        QSet<QByteArray> keys;
        while (true)
        {
            const auto token = fetch();
            if (token.type == Type::DictionaryEnd)
                break;
            if (token.type != Type::Name)
                invalid();
            const auto key = token.data.toByteArray();
            if (keys.contains(key))
                invalid();
            keys.insert(key);
            const auto value = fetch();
            if (key == "Registry" || key == "Ordering")
            {
                if (value.type != Type::String || value.data.toByteArray().isEmpty())
                    invalid();
            }
            else if (key == "Supplement")
            {
                if (value.type != Type::Integer || value.data.toLongLong() < 0 ||
                    value.data.toLongLong() > 65535)
                    invalid();
            }
            else
                invalid();
        }
        if (keys.size() != 3)
            invalid();
    }
    void readMetadata(const QByteArray& name)
    {
        if (metadata.contains(name))
            invalid();
        metadata.insert(name);
        if (name == "CIDSystemInfo")
            readSystemInfo();
        else if (name == "CMapName")
        {
            const auto value = fetch();
            if (value.type != Type::Name || value.data.toByteArray().isEmpty() ||
                value.data.toByteArray() == "UseCMap")
                invalid();
        }
        else if (name == "WMode" || name == "CMapType")
        {
            const auto value = fetch();
            const int expected = name == "WMode" ? 0 : unicode ? 2 : 1;
            if (value.type != Type::Integer || value.data.toLongLong() != expected)
                invalid();
        }
        else
            invalid();
        expect(Type::Command, "def");
    }
    quint16 mappedValue(const Token& token) const
    {
        if (unicode)
            return unicodeValue(token);
        if (token.type != Type::Integer || token.data.toLongLong() < 0 ||
            token.data.toLongLong() > 65535)
            invalid();
        return quint16(token.data.toLongLong());
    }
    void readBlock(qint64 rows)
    {
        const auto token = fetch();
        if (token.type != Type::Command || rows < 1 || rows > 8192)
            invalid();
        const auto command = token.data.toByteArray();
        const bool space = command == "begincodespacerange";
        const bool single = command == (unicode ? "beginbfchar" : "begincidchar");
        const bool range = command == (unicode ? "beginbfrange" : "begincidrange");
        if (!space && !single && !range)
            invalid();
        for (qint64 row = 0; row < rows; ++row)
        {
            const auto from = sourceCode(fetch());
            const auto to = single ? from : sourceCode(fetch());
            if (from.first != to.first || from.second > to.second)
                invalid();
            if (space)
            {
                if ((from.first == 2 && (from.second & 255) > (to.second & 255)) ||
                    result.spaces.size() >= 256)
                    invalid();
                const CodeSpace entry{from, to};
                for (const auto& other : result.spaces)
                    if (entry.conflicts(other))
                        invalid();
                result.spaces << entry;
                continue;
            }
            const auto target = fetch();
            const bool array = unicode && range && target.type == Type::ArrayStart;
            if (int(to.second) - from.second >= 8192)
                invalid();
            const int first = array ? 0 : mappedValue(target);
            for (int source = from.second; source <= to.second; ++source)
            {
                const int mapped = array ? mappedValue(fetch()) : first + source - from.second;
                const SourceCode key{from.first, quint16(source)};
                if (mapped > 65535 || (unicode && QChar(ushort(mapped)).isSurrogate()) ||
                    result.values.contains(key) || result.values.size() >= 8192)
                    invalid();
                result.values[key] = quint16(mapped);
            }
            if (array && fetch().type != Type::ArrayEnd)
                invalid();
        }
        expect(Type::Command, "end" + command.mid(5));
    }
    QVector<Token> tokens;
    qsizetype index = 0;
    bool unicode;
    std::function<bool()> cancelled;
    QSet<QByteArray> metadata;
    Map result;
};
Map readMap(const PDFDocument& document, const PDFObject& reference, bool unicode,
            const std::function<bool()>& cancelled)
{
    const auto object = document.getObject(reference);
    if (!object.isStream())
        invalid();
    const auto stream = object.getStream();
    validateStreamDictionary(document, *stream->getDictionary());
    if (stream->getContent()->size() > 1024 * 1024)
        invalid();
    const auto bytes = document.getDecodedStream(stream);
    if (bytes.size() > 1024 * 1024)
        invalid();
    PDFLexicalAnalyzer parser(bytes.constBegin(), bytes.constEnd());
    QVector<Token> tokens;
    while (true)
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
        parser.skipWhitespaceAndComments();
        if (parser.isAtEnd())
            break;
        if (tokens.size() >= 65536)
            invalid();
        const auto token = parser.fetch();
        if (token.type == Type::EndOfFile)
            invalid();
        tokens << token;
    }
    return MapReader(std::move(tokens), unicode, cancelled).read();
}
} // namespace
RedactionCMap::RedactionCMap(const PDFDocument& document, const PDFDictionary& font,
                             const std::function<bool()>& cancelled)
    : cancelled(cancelled)
{
    const auto encoding = readMap(document, font.get("Encoding"), false, cancelled);
    const auto unicode = readMap(document, font.get("ToUnicode"), true, cancelled);
    if (encoding.values.size() != unicode.values.size())
        invalid();
    QMap<quint16, quint16> meanings;
    QMap<quint16, quint8> sourceLengths;
    for (auto it = encoding.values.cbegin(); it != encoding.values.cend(); ++it)
    {
        if (!unicode.values.contains(it.key()))
            invalid();
        const auto character = unicode.values[it.key()];
        if (meanings.contains(it.value()) && meanings[it.value()] != character)
            invalid();
        meanings[it.value()] = character;
        const auto source = it.key();
        if (sourceLengths.contains(source.second) && sourceLengths[source.second] != source.first)
            numericCodeLengthCollision = true;
        sourceLengths[source.second] = source.first;
        twoByteCode32 = twoByteCode32 || (source.first == 2 && source.second == 32);
        codes[source] = {it.value(), QChar(character), source.first, source.second};
    }
    for (const auto& space : encoding.spaces)
    {
        singleByteCodes = singleByteCodes || space.from.first == 1;
        for (int prefix = space.firstByte(); prefix <= space.lastByte(); ++prefix)
            codeLengths[quint8(prefix)] = space.from.first;
    }
}
QVector<RedactionCMap::Code> RedactionCMap::decode(const QByteArray& bytes) const
{
    QVector<Code> result;
    for (qsizetype at = 0; at < bytes.size();)
        result << nextCode(bytes, at);
    return result;
}
void RedactionCMap::validate(const QByteArray& bytes) const
{
    for (qsizetype at = 0; at < bytes.size();)
        nextCode(bytes, at);
}
RedactionCMap::Code RedactionCMap::nextCode(const QByteArray& bytes, qsizetype& at) const
{
    if (cancelled && cancelled())
        fail("墨消しを中止しました。文書は変更していません。");
    const auto prefix = quint8(bytes[at]);
    const auto length = codeLengths.value(prefix, 0);
    if (!length || at + length > bytes.size())
        invalid();
    const quint16 value = length == 1 ? prefix : qFromBigEndian<quint16>(bytes.constData() + at);
    const SourceCode source{length, value};
    const auto code = codes.constFind(source);
    if (code == codes.cend() || !code->cid || code->character.isNull())
        invalid();
    at += length;
    return code.value();
}
} // namespace tatsu
