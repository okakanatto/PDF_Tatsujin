#include "pdf_optimization.h"
#include "pdf_objects.h"
#include "pdfobjectutils.h"
#include "pdfoptimizer.h"
#include <array>
#include <map>
#include <zlib.h>

namespace tatsu
{
using namespace detail;
namespace
{
constexpr qsizetype inputLimit = 64 * 1024 * 1024;
constexpr qsizetype outputLimit = 128 * 1024 * 1024;
constexpr qint64 totalLimit = 512ll * 1024 * 1024;
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("容量の最適化を中止しました。文書は変更していません。");
}
QByteArray flate(const QByteArray& input, bool compress, const std::function<bool()>& cancelled)
{
    if ((!compress && input.size() > inputLimit) || (compress && input.size() > outputLimit))
        fail("Flateストリームが今回の容量最適化の処理上限を超えています。");
    z_stream stream{};
    const int init = compress ? deflateInit(&stream, Z_BEST_COMPRESSION) : inflateInit(&stream);
    if (init != Z_OK)
        fail("Flate処理を初期化できません。");
    struct Cleanup
    {
        z_stream* stream;
        bool compress;
        ~Cleanup()
        {
            if (compress)
                deflateEnd(stream);
            else
                inflateEnd(stream);
        }
    } cleanup{&stream, compress};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    stream.avail_in = uInt(input.size());
    QByteArray result;
    std::array<char, 65536> buffer;
    for (;;)
    {
        stop(cancelled);
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = uInt(buffer.size());
        const int status = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END)
            fail("Flateデータが不正なため最適化できません。");
        const auto count = qsizetype(buffer.size() - stream.avail_out);
        if (result.size() + count > outputLimit)
            fail("Flate展開が128MiBの処理上限を超えています。");
        result.append(buffer.data(), count);
        if (status == Z_STREAM_END)
        {
            if (stream.avail_in)
                fail("Flateデータの末尾に未処理データがあります。");
            return result;
        }
    }
}
bool eligible(const PDFObjectStorage& storage, const PDFStream* stream)
{
    const auto dictionary = stream->getDictionary();
    if (dictionary->hasKey("F") || dictionary->hasKey("StructParent") ||
        dictionary->hasKey("StructParents"))
        return false;
    const auto type = storage.getObject(dictionary->get("Type"));
    return type.isNull() || (type.isName() && type.getString() == "XObject");
}
bool singleFlate(const PDFObjectStorage& storage, const PDFStream* stream)
{
    auto filter = storage.getObject(stream->getDictionary()->get("Filter"));
    if (filter.isArray() && filter.getArray()->getCount() == 1)
        filter = storage.getObject(filter.getArray()->getItem(0));
    return filter.isName() && (filter.getString() == "FlateDecode" || filter.getString() == "Fl");
}
int occupied(const PDFObjectStorage& storage)
{
    int result = 0;
    for (const auto& entry : storage.getObjects())
        if (!entry.object.isNull())
            ++result;
    return result;
}
int shareStreams(PDFObjectStorage& storage, const std::function<bool()>& cancelled)
{
    std::map<QByteArray, std::vector<PDFObjectReference>> candidates;
    std::map<PDFObjectReference, PDFObjectReference> replacements;
    auto& objects = storage.getObjects();
    for (size_t index = 1; index < objects.size(); ++index)
    {
        stop(cancelled);
        const auto& entry = objects[index];
        if (!entry.object.isStream() || !eligible(storage, entry.object.getStream()))
            continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(*entry.object.getStream()->getContent());
        const PDFObjectReference reference(PDFInteger(index), entry.generation);
        auto& bucket = candidates[hash.result()];
        bool shared = false;
        for (auto previous : bucket)
            if (entry.object == storage.getObjectByReference(previous))
            {
                replacements.emplace(reference, previous);
                shared = true;
                break;
            }
        if (!shared)
            bucket.push_back(reference);
    }
    if (!replacements.empty())
    {
        for (auto& entry : objects)
        {
            stop(cancelled);
            entry.object = PDFObjectUtils::replaceReferences(entry.object, replacements);
        }
        storage.setTrailerDictionary(
            PDFObjectUtils::replaceReferences(storage.getTrailerDictionary(), replacements));
    }
    return int(replacements.size());
}
PDFObjectStorage compact(PDFObjectStorage storage, bool shrink,
                         const std::function<bool()>& cancelled)
{
    PDFOptimizer optimizer(PDFOptimizer::RemoveUnusedObjects |
                               (shrink ? PDFOptimizer::ShrinkObjectStorage : PDFOptimizer::None),
                           nullptr);
    optimizer.setStorage(storage);
    QObject::connect(
        &optimizer, &PDFOptimizer::optimizationProgress, &optimizer,
        [&](const QString&) { stop(cancelled); }, Qt::DirectConnection);
    optimizer.optimize();
    stop(cancelled);
    return optimizer.takeStorage();
}
} // namespace
OptimizationResult optimizePdf(const PDFDocument& document, const std::function<bool()>& cancelled,
                               const std::function<void(QString)>& progress)
{
    if (!document.getCatalog() || !document.getCatalog()->getPageCount() ||
        !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    stop(cancelled);
    OptimizationResult result;
    if (progress)
        progress("通常保存した場合のサイズを確認しています…");
    result.beforeBytes = encodePdf(document).size();
    stop(cancelled);
    auto storage = compact(document.getStorage(), false, cancelled);
    const int originalCount = occupied(document.getStorage());
    auto& objects = storage.getObjects();
    qint64 expanded = 0;
    for (size_t index = 1; index < objects.size(); ++index)
    {
        stop(cancelled);
        auto& entry = objects[index];
        if (!entry.object.isStream() || !eligible(storage, entry.object.getStream()))
            continue;
        if (progress && index % 32 == 0)
            progress(QString("データを整理しています… %1 / %2").arg(index).arg(objects.size()));
        auto stream = entry.object.getStream();
        if (singleFlate(storage, stream))
        {
            const auto decoded = flate(*stream->getContent(), false, cancelled);
            expanded += decoded.size();
            if (expanded > totalLimit)
                fail("Flate展開の合計が512MiBの処理上限を超えています。");
            const auto compressed = flate(decoded, true, cancelled);
            if (compressed.size() < stream->getContent()->size())
            {
                if (flate(compressed, false, cancelled) != decoded)
                    fail("再圧縮の完全一致を確認できません。");
                entry.object = streamObject(*stream->getDictionary(), compressed);
            }
        }
    }
    if (progress)
        progress("参照を更新し、未使用データを回収しています…");
    // Sharing a font/image can make its parent appearance streams identical.
    // Resolve that dependency to a fixed point without decoding streams again.
    for (;;)
    {
        stop(cancelled);
        const int shared = shareStreams(storage, cancelled);
        result.sharedStreams += shared;
        storage = compact(std::move(storage), true, cancelled);
        if (!shared)
            break;
    }
    result.removedObjects = originalCount - occupied(storage) - result.sharedStreams;
    PDFDocument candidate(std::move(storage), document.getInfo()->version,
                          document.getSourceDataHash());
    stop(cancelled);
    if (candidate.getCatalog()->getPageCount() != document.getCatalog()->getPageCount())
        fail("最適化後のページ数が一致しません。");
    if (progress)
        progress("最適化後の通常PDFを検査しています…");
    result.afterBytes = encodePdf(candidate).size();
    stop(cancelled);
    result.document = result.smaller() ? std::move(candidate) : document;
    return result;
}
} // namespace tatsu
