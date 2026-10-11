#include "redaction_resource_audit.h"
#include <QCryptographicHash>
#include <array>
#include <zlib.h>

namespace tatsu
{
namespace
{
using namespace pdf;
constexpr qsizetype inputLimit = 64 * 1024 * 1024;
constexpr qint64 streamLimit = 128ll * 1024 * 1024;
constexpr qint64 totalLimit = 512ll * 1024 * 1024;
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("墨消しを中止しました。文書は変更していません。");
}
QByteArray ascii85(const QByteArray& input, const std::function<bool()>& cancelled)
{
    QByteArray output;
    quint64 tuple = 0;
    int digits = 0;
    bool ended = false;
    qsizetype position = 0;
    auto whitespace = [](char value) {
        return value == 0 || value == 9 || value == 10 || value == 12 || value == 13 || value == 32;
    };
    while (position < input.size() && whitespace(input[position]))
        ++position;
    if (input.mid(position, 2) == "<~")
        position += 2;
    auto append = [&](quint64 value, int count)
    {
        if (value > 0xffffffffull || output.size() > inputLimit - count)
            fail("サブセット書体の複製検査で不正なASCII85または入力上限を検出しました。");
        for (int index = 0; index < count; ++index)
            output.append(char((value >> (24 - index * 8)) & 255));
    };
    for (; position < input.size(); ++position)
    {
        if (position % 1024 == 0)
            stop(cancelled);
        const auto value = input[position];
        if (whitespace(value))
            continue;
        if (value == '~')
        {
            if (position + 1 >= input.size() || input[++position] != '>')
                fail("サブセット書体の複製検査で不正なASCII85終端を検出しました。");
            ended = true;
            ++position;
            break;
        }
        if (value == 'z')
        {
            if (digits)
                fail("サブセット書体の複製検査で不正なASCII85短縮を検出しました。");
            append(0, 4);
            continue;
        }
        if (value < '!' || value > 'u')
            fail("サブセット書体の複製検査で不正なASCII85文字を検出しました。");
        tuple = tuple * 85 + quint8(value - '!');
        if (++digits == 5)
        {
            append(tuple, 4);
            tuple = 0;
            digits = 0;
        }
    }
    if (!ended || digits == 1)
        fail("サブセット書体の複製検査で未完のASCII85を検出しました。");
    if (digits)
    {
        const int count = digits - 1;
        while (digits++ < 5)
            tuple = tuple * 85 + 84;
        append(tuple, count);
    }
    for (; position < input.size(); ++position)
        if (!whitespace(input[position]))
            fail("サブセット書体の複製検査でASCII85末尾データを検出しました。");
    return output;
}
QByteArray streamHash(const PDFObjectStorage& storage, const PDFStream* value, qint64& total,
                      const std::function<bool()>& cancelled)
{
    const auto dictionary = value->getDictionary();
    const auto filter = storage.getObject(dictionary->get("Filter"));
    QVector<QByteArray> filters;
    auto addFilter = [&](const PDFObject& item)
    {
        const auto entry = storage.getObject(item);
        if (!entry.isName())
            fail("サブセット書体の複製検査で不正なフィルターを検出しました。");
        filters << entry.getString();
    };
    if (filter.isArray())
    {
        if (filter.getArray()->getCount() > 2)
            fail("サブセット書体の複製検査のフィルター上限を超えています。");
        for (const auto& item : *filter.getArray())
            addFilter(item);
    }
    else if (!filter.isNull())
        addFilter(filter);
    const bool a85 =
        !filters.isEmpty() && (filters.first() == "ASCII85Decode" || filters.first() == "A85");
    if (a85)
        filters.removeFirst();
    const bool flate =
        filters.size() == 1 && (filters.first() == "FlateDecode" || filters.first() == "Fl");
    if (dictionary->hasKey("F") || !storage.getObject(dictionary->get("DecodeParms")).isNull() ||
        (!filters.isEmpty() && !flate))
        fail("サブセット書体の複製を確認できないストリーム形式です。候補は出力していません。");
    if (value->getContent()->size() > inputLimit)
        fail("サブセット書体の複製検査がストリーム入力上限を超えています。");
    const auto decodedAscii = a85 ? ascii85(*value->getContent(), cancelled) : QByteArray();
    const auto& input = a85 ? decodedAscii : *value->getContent();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 decoded = 0;
    auto append = [&](QByteArrayView part)
    {
        if (part.size() > streamLimit - decoded || part.size() > totalLimit - total)
            fail("サブセット書体の複製検査が展開上限を超えています。候補は出力していません。");
        decoded += part.size();
        total += part.size();
        hash.addData(part);
    };
    if (!flate)
    {
        for (qsizetype offset = 0; offset < input.size(); offset += 65536)
        {
            stop(cancelled);
            append(QByteArrayView(input).sliced(offset,
                                                qMin(qsizetype(65536), input.size() - offset)));
        }
        return hash.result();
    }
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
        fail("サブセット書体の複製検査を初期化できません。");
    struct Cleanup
    {
        z_stream* stream;
        ~Cleanup()
        {
            inflateEnd(stream);
        }
    } cleanup{&stream};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    stream.avail_in = uInt(input.size());
    std::array<char, 65536> buffer;
    for (;;)
    {
        stop(cancelled);
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = uInt(buffer.size());
        const auto status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END)
            fail("サブセット書体の複製検査で不正なFlateデータを検出しました。");
        append(QByteArrayView(buffer.data(), qsizetype(buffer.size() - stream.avail_out)));
        if (status == Z_STREAM_END)
        {
            if (stream.avail_in)
                fail("サブセット書体の複製検査でFlate末尾の未処理データを検出しました。");
            return hash.result();
        }
    }
}
} // namespace
void rejectRetainedRedactionResources(const PDFDocument& source,
                                      const pdf::PDFObjectStorage& candidate,
                                      const std::set<pdf::PDFObjectReference>& images,
                                      const std::set<pdf::PDFObjectReference>& removedDependencies,
                                      const std::function<bool()>& cancelled)
{
    if (images.empty() && removedDependencies.empty())
        return;
    std::set<QByteArray> removedFontStreams;
    qint64 decodedTotal = 0;
    for (const auto& reference : removedDependencies)
    {
        stop(cancelled);
        const auto object = source.getObjectByReference(reference);
        if (object.isStream())
            removedFontStreams.insert(
                streamHash(source.getStorage(), object.getStream(), decodedTotal, cancelled));
    }
    std::set<pdf::PDFObjectReference> visited;
    std::vector<pdf::PDFObject> pending{candidate.getTrailerDictionary()};
    size_t examined = 0;
    auto appendDictionary = [&](const pdf::PDFDictionary* dictionary)
    {
        if (dictionary->getCount() > 1000000 - examined ||
            pending.size() + dictionary->getCount() > 1000000)
            fail("資源の共有参照が処理上限を超えています。墨消ししていません。");
        for (size_t i = 0; i < dictionary->getCount(); ++i)
            pending.push_back(dictionary->getValue(i));
    };
    while (!pending.empty())
    {
        stop(cancelled);
        if (++examined > 1000000)
            fail("資源の共有参照が処理上限を超えています。墨消ししていません。");
        auto object = std::move(pending.back());
        pending.pop_back();
        if (object.isReference())
        {
            const auto reference = object.getReference();
            if (images.contains(reference))
                fail("墨消し対象の元画像が別の共有参照に残ります。このPDFはまだ安全に処理できません"
                     "。");
            if (removedDependencies.contains(reference))
                fail("削除したサブセット書体の情報が共有参照に残ります。候補は出力していません。");
            if (visited.insert(reference).second)
                pending.push_back(candidate.getObjectByReference(reference));
        }
        else if (object.isDictionary())
            appendDictionary(object.getDictionary());
        else if (object.isStream())
        {
            if (!removedFontStreams.empty() &&
                removedFontStreams.contains(
                    streamHash(candidate, object.getStream(), decodedTotal, cancelled)))
                fail("削除したサブセット書体の複製データが残ります。候補は出力していません。");
            appendDictionary(object.getStream()->getDictionary());
        }
        else if (object.isArray())
        {
            const auto array = object.getArray();
            if (pending.size() + array->getCount() > 1000000)
                fail("資源の共有参照が処理上限を超えています。墨消ししていません。");
            for (const auto& item : *array)
                pending.push_back(item);
        }
    }
}
} // namespace tatsu
