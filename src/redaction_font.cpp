#include "redaction_font.h"
#include "pdf_objects.h"
#include "pdffont.h"
#include "redaction_hint_program.h"
#include "redaction_sfnt_metrics.h"
#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>

namespace tatsu
{
namespace
{
using namespace pdf;
using namespace detail;
void invalid()
{
    fail("このTrueType字体を安全に再構築できません。墨消し候補は保存していません。");
}
quint16 u16(const QByteArray& data, qsizetype at)
{
    if (at < 0 || at > data.size() - 2)
        invalid();
    return qFromBigEndian<quint16>(data.constData() + at);
}
quint32 u32(const QByteArray& data, qsizetype at)
{
    if (at < 0 || at > data.size() - 4)
        invalid();
    return qFromBigEndian<quint32>(data.constData() + at);
}
void put16(QByteArray& data, qsizetype at, quint16 value)
{
    if (at < 0 || at > data.size() - 2)
        invalid();
    qToBigEndian<quint16>(value, data.data() + at);
}
void put32(QByteArray& data, qsizetype at, quint32 value)
{
    if (at < 0 || at > data.size() - 4)
        invalid();
    qToBigEndian<quint32>(value, data.data() + at);
}
void append16(QByteArray& data, quint16 value)
{
    data.append(char(value >> 8));
    data.append(char(value));
}
void append32(QByteArray& data, quint32 value)
{
    append16(data, quint16(value >> 16));
    append16(data, quint16(value));
}
quint32 checksum(QByteArray data)
{
    while (data.size() % 4)
        data.append('\0');
    quint32 sum = 0;
    for (qsizetype i = 0; i < data.size(); i += 4)
        sum += u32(data, i);
    return sum;
}
struct Subset
{
    QByteArray bytes;
    QMap<quint16, quint16> gids;
    QSet<quint16> cvtReferences;
    bool hasCvt = false;
};
Subset subset(const QByteArray& input, const QVector<RedactionGlyph>& glyphs,
              const std::function<bool()>& cancelled, bool sharedCvtUsage,
              RedactionFontBounds bounds)
{
    auto stop = [&]
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
    };
    stop();
    if (input.size() > 16 * 1024 * 1024 || u32(input, 0) != 0x10000)
        invalid();
    const int count = u16(input, 4);
    if (!count || count > 128 || input.size() < 12 + count * 16)
        invalid();
    QMap<QByteArray, QByteArray> tables;
    QVector<QPair<qsizetype, qsizetype>> ranges;
    for (int i = 0; i < count; ++i)
    {
        const int entry = 12 + i * 16;
        const auto tag = input.mid(entry, 4);
        const qsizetype offset = u32(input, entry + 8), length = u32(input, entry + 12);
        if (tables.contains(tag) || offset < 12 + count * 16 || offset % 4 ||
            offset > input.size() || length > input.size() - offset)
            invalid();
        tables[tag] = input.mid(offset, length);
        if (length)
            ranges.append({offset, offset + length});
    }
    std::sort(ranges.begin(), ranges.end());
    for (int i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second)
            invalid();
    for (const auto tag : {"head", "hhea", "maxp", "hmtx", "loca", "glyf"})
        if (!tables.contains(tag))
            invalid();
    if (tables.contains("CFF ") || tables.contains("gvar") || tables.contains("fvar"))
        invalid();
    validateRedactionHintTables(tables);
    auto head = tables["head"], hhea = tables["hhea"], maxp = tables["maxp"];
    if (head.size() != 54 || hhea.size() != 36 || maxp.size() != 32 || u32(maxp, 0) != 0x10000 ||
        u32(head, 12) != 0x5f0f3cf5 || u32(head, 0) != 0x10000 || u32(hhea, 0) != 0x10000 ||
        u16(head, 52) != 0 || u16(hhea, 32) != 0)
        invalid();
    const int total = u16(maxp, 4), metrics = u16(hhea, 34), locaFormat = u16(head, 50);
    if (!total || !metrics || metrics > total || locaFormat > 1 ||
        tables["hmtx"].size() < metrics * 4 + (total - metrics) * 2 ||
        tables["loca"].size() < (total + 1) * (locaFormat ? 4 : 2))
        invalid();
    QVector<quint32> offsets;
    for (int i = 0; i <= total; ++i)
    {
        const auto offset =
            locaFormat ? u32(tables["loca"], i * 4) : quint32(u16(tables["loca"], i * 2)) * 2;
        if (offset > quint32(tables["glyf"].size()) ||
            (!offsets.isEmpty() && offset < offsets.last()))
            invalid();
        offsets << offset;
    }
    QMap<quint16, QByteArray> retained;
    QMap<quint16, RedactionGlyphMetrics> statistics;
    QMap<quint16, QVector<int>> components;
    QMap<quint16, RedactionGlyphHints> glyphHints;
    QMap<quint16, qsizetype> instructionOffsets;
    QSet<quint16> active;
    bool requiredNotdef = false;
    std::function<void(quint16, int)> visit = [&](quint16 gid, int depth)
    {
        stop();
        if (gid >= total || depth > 32 || active.contains(gid))
            invalid();
        if (gid == 0 && depth && !requiredNotdef)
        {
            // GID 0 may be a real component of a surviving compound glyph.
            // Replace the default empty placeholder with its parsed closure.
            requiredNotdef = true;
            retained.remove(0);
            statistics.remove(0);
        }
        if (retained.contains(gid))
            return;
        if (retained.size() >= 8192)
            invalid();
        active.insert(gid);
        auto data = tables["glyf"].mid(offsets[gid], offsets[gid + 1] - offsets[gid]);
        RedactionGlyphMetrics stats;
        qsizetype end = 0;
        if (!data.isEmpty())
        {
            if (data.size() < 10)
                invalid();
            const qint16 contours = qint16(u16(data, 0));
            for (int i = 0; i < 4; ++i)
                stats.bounds[i] = qint16(u16(data, 2 + 2 * i));
            if (contours == 0)
            {
                // Empty outlines have no meaningful bbox or point encoding.
                // Instructions on phantom points remain unsupported.
                if (data.size() > 15)
                    invalid();
                for (qsizetype i = 10; i < data.size(); ++i)
                    if (data[i])
                        invalid();
                end = data.size();
            }
            else if (contours > 0)
            {
                qsizetype at = 10 + contours * 2;
                int points = contours ? u16(data, at - 2) + 1 : 0;
                quint16 previous = 0;
                for (int i = 0; i < contours; ++i)
                {
                    const auto endpoint = u16(data, 10 + i * 2);
                    if (i && endpoint <= previous)
                        invalid();
                    previous = endpoint;
                }
                const auto instructionBytes = u16(data, at);
                at += 2;
                if (instructionBytes > data.size() - at)
                    invalid();
                const auto hints = validateRedactionGlyphHints(
                    data.mid(at, instructionBytes), points, tables.value("cvt ").size() / 2);
                glyphHints[gid] = hints;
                instructionOffsets[gid] = at;
                stats.instructionBytes = hints.instructionBytes;
                stats.maxStackElements = hints.maxStackElements;
                at += instructionBytes;
                stats.points = points;
                stats.contours = contours;
                QVector<quint8> flags;
                flags.reserve(points);
                qsizetype coordinates = 0;
                for (int i = 0; i < points;)
                {
                    if (at >= data.size())
                        invalid();
                    const auto flag = quint8(data[at++]);
                    if (flag & 0xc0)
                        invalid();
                    int repetitions = 1;
                    if (flag & 8)
                    {
                        if (at >= data.size())
                            invalid();
                        repetitions += quint8(data[at++]);
                    }
                    if (repetitions > points - i)
                        invalid();
                    coordinates += repetitions * ((flag & 2    ? 1
                                                   : flag & 16 ? 0
                                                               : 2) +
                                                  (flag & 4    ? 1
                                                   : flag & 32 ? 0
                                                               : 2));
                    for (int j = 0; j < repetitions; ++j)
                        flags << flag;
                    i += repetitions;
                }
                end = at + coordinates;
                if (end > data.size())
                    invalid();
                std::array<qint16, 4> actual{};
                for (const int axis : {0, 1})
                {
                    qint32 position = 0;
                    for (int i = 0; i < points; ++i)
                    {
                        const auto flag = flags[i];
                        const int shortMask = axis ? 4 : 2, sameMask = axis ? 32 : 16;
                        if (flag & shortMask)
                        {
                            const int delta = quint8(data[at++]);
                            position += flag & sameMask ? delta : -delta;
                        }
                        else if (!(flag & sameMask))
                        {
                            position += qint16(u16(data, at));
                            at += 2;
                        }
                        if (position < -32768 || position > 32767)
                            invalid();
                        if (!i)
                            actual[axis] = actual[axis + 2] = qint16(position);
                        else
                        {
                            actual[axis] = qMin(actual[axis], qint16(position));
                            actual[axis + 2] = qMax(actual[axis + 2], qint16(position));
                        }
                    }
                }
                if (at != end || actual != stats.bounds)
                    invalid();
            }
            else if (contours == -1)
            {
                qsizetype at = 10;
                stats.composite = true;
                quint16 flags;
                do
                {
                    flags = u16(data, at);
                    if (flags & ~quint16(0x1fef) || flags & 0x100)
                        invalid();
                    components[gid] << int(at + 2);
                    visit(u16(data, at + 2), depth + 1);
                    const auto child = statistics[u16(data, at + 2)];
                    stats.points += child.points;
                    stats.contours += child.contours;
                    stats.componentDepth =
                        qMax(stats.componentDepth, quint16(child.componentDepth + 1));
                    if (++stats.componentCount > 4096 || stats.points > 65535 ||
                        stats.contours > 65535)
                        invalid();
                    at += 4 + (flags & 1 ? 4 : 2);
                    int transforms = 0;
                    if (flags & 8)
                    {
                        at += 2;
                        ++transforms;
                    }
                    if (flags & 0x40)
                    {
                        at += 4;
                        ++transforms;
                    }
                    if (flags & 0x80)
                    {
                        at += 8;
                        ++transforms;
                    }
                    if (transforms > 1 || at > data.size())
                        invalid();
                } while (flags & 0x20);
                end = at;
            }
            else
                invalid();
            if (end > data.size() || data.size() - end > 3)
                invalid();
            for (qsizetype i = end; i < data.size(); ++i)
                if (data[i])
                    invalid();
            data.truncate(end);
            if (contours == 0)
                data.clear();
        }
        retained[gid] = data;
        statistics[gid] = stats;
        active.remove(gid);
    };
    // Keep .notdef empty unless a surviving compound glyph actually needs it.
    retained[0] = {};
    for (const auto& glyph : glyphs)
    {
        if (!glyph.gid || glyph.character.isNull() || glyph.character.isSurrogate())
            invalid();
        visit(glyph.gid, 0);
    }
    QMap<quint16, quint16> cvtIndices;
    for (const auto& hints : glyphHints)
    {
        stop();
        for (const auto index : hints.cvtOperands)
            cvtIndices.insert(index, 0);
    }
    const auto originalCvt = tables.value("cvt ");
    if (!originalCvt.isEmpty() && cvtIndices.isEmpty() && !sharedCvtUsage)
        invalid(); // A standalone unused CVT remains outside the frozen grammar.
    QByteArray cvt;
    for (auto it = cvtIndices.begin(); it != cvtIndices.end(); ++it)
    {
        stop();
        it.value() = quint16(cvt.size() / 2);
        // Copy the exact signed FWORD, preserving equal values at distinct indices.
        cvt += originalCvt.mid(it.key() * 2, 2);
    }
    Subset result;
    result.hasCvt = !originalCvt.isEmpty();
    for (auto it = cvtIndices.cbegin(); it != cvtIndices.cend(); ++it)
        result.cvtReferences.insert(it.key());
    quint16 next = 0;
    for (auto it = retained.cbegin(); it != retained.cend(); ++it)
        result.gids[it.key()] = next++;
    QByteArray glyf, loca, hmtx;
    QVector<RedactionGlyphMetrics> metricsRows;
    for (auto it = retained.cbegin(); it != retained.cend(); ++it)
    {
        stop();
        append32(loca, glyf.size());
        auto data = it.value();
        auto stats = statistics[it.key()];
        const auto hints = glyphHints.value(it.key());
        if (!hints.cvtOperands.isEmpty())
        {
            const auto at = instructionOffsets.value(it.key());
            const auto rewritten =
                rewriteRedactionGlyphHints(data.mid(at, hints.instructionBytes), hints, cvtIndices);
            data.replace(at, hints.instructionBytes, rewritten);
            put16(data, at - 2, quint16(rewritten.size()));
            stats.instructionBytes = quint16(rewritten.size());
        }
        for (auto at : components.value(it.key()))
            put16(data, at, result.gids[u16(data, at)]);
        glyf += data;
        while (glyf.size() % 4)
            glyf.append('\0');
        const int gid = it.key();
        append16(hmtx,
                 gid == 0 && !requiredNotdef ? 0 : u16(tables["hmtx"], qMin(gid, metrics - 1) * 4));
        append16(hmtx, data.isEmpty()
                           ? 0
                           : u16(tables["hmtx"],
                                 gid < metrics ? gid * 4 + 2 : metrics * 4 + (gid - metrics) * 2));
        stats.advance = u16(hmtx, hmtx.size() - 4);
        stats.bearing = qint16(u16(hmtx, hmtx.size() - 2));
        metricsRows << stats;
    }
    append32(loca, glyf.size());
    const auto originalRenderingBounds = head.mid(36, 8);
    if (bounds == RedactionFontBounds::EmbeddedCMapSource)
    {
        std::array<qint16, 4> box;
        for (size_t index = 0; index < box.size(); ++index)
            box[index] = qint16(u16(head, 36 + index * 2));
        if (box[0] > box[2] || box[1] > box[3])
            invalid();
        for (const auto& glyph : metricsRows)
            if (glyph.contours && (glyph.bounds[0] < box[0] || glyph.bounds[1] < box[1] ||
                                   glyph.bounds[2] > box[2] || glyph.bounds[3] > box[3]))
                invalid();
    }
    else if (bounds != RedactionFontBounds::RetainedGlyphs)
        invalid();
    const auto headers = rebuildRedactionSfntHeaders(head, hhea, metricsRows);
    head = headers.head;
    // Global head bounds affect low-DPI rasterization even with unchanged
    // outlines. A qualified embedded CMap keeps those validated render inputs.
    if (bounds == RedactionFontBounds::EmbeddedCMapSource)
        head.replace(36, 8, originalRenderingBounds);
    hhea = headers.hhea;
    maxp = headers.maxp;
    if (!tables.value("prep").isEmpty())
        put16(maxp, 24, qMax(quint16(1), u16(maxp, 24)));
    QByteArray cmap;
    append16(cmap, 0);
    append16(cmap, 1);
    append16(cmap, 3);
    append16(cmap, 10);
    append32(cmap, 12);
    append16(cmap, 12);
    append16(cmap, 0);
    QMap<quint16, quint16> unicode;
    for (const auto& glyph : glyphs)
    {
        const auto cp = glyph.character.unicode();
        const auto gid = result.gids[glyph.gid];
        if (unicode.contains(cp) && unicode[cp] != gid)
            invalid();
        unicode[cp] = gid;
    }
    append32(cmap, 16 + 12 * unicode.size());
    append32(cmap, 0);
    append32(cmap, unicode.size());
    for (auto it = unicode.cbegin(); it != unicode.cend(); ++it)
    {
        append32(cmap, it.key());
        append32(cmap, it.key());
        append32(cmap, it.value());
    }
    QByteArray name(6, '\0');
    QByteArray post(32, '\0');
    put32(post, 0, 0x30000);
    QMap<QByteArray, QByteArray> output{{"head", head}, {"hhea", hhea}, {"maxp", maxp},
                                        {"glyf", glyf}, {"loca", loca}, {"hmtx", hmtx},
                                        {"cmap", cmap}, {"name", name}, {"post", post}};
    if (!tables.value("prep").isEmpty())
        output["prep"] = tables["prep"];
    if (!cvt.isEmpty())
        output["cvt "] = cvt;
    result.bytes = QByteArray(12 + 16 * output.size(), '\0');
    put32(result.bytes, 0, 0x10000);
    put16(result.bytes, 4, output.size());
    int power = 1, selector = 0;
    while (power * 2 <= output.size())
    {
        power *= 2;
        ++selector;
    }
    put16(result.bytes, 6, power * 16);
    put16(result.bytes, 8, selector);
    put16(result.bytes, 10, output.size() * 16 - power * 16);
    int entry = 12;
    qsizetype headOffset = 0;
    for (auto it = output.cbegin(); it != output.cend(); ++it, entry += 16)
    {
        result.bytes.replace(entry, 4, it.key());
        put32(result.bytes, entry + 4, checksum(it.value()));
        put32(result.bytes, entry + 8, result.bytes.size());
        put32(result.bytes, entry + 12, it.value().size());
        if (it.key() == "head")
            headOffset = result.bytes.size();
        result.bytes += it.value();
        while (result.bytes.size() % 4)
            result.bytes.append('\0');
    }
    put32(result.bytes, headOffset + 8, 0xb1b0afba - checksum(result.bytes));
    return result;
}
PDFObjectReference compressed(PDFDocumentBuilder& builder, PDFDictionary dictionary,
                              const QByteArray& data)
{
    set(dictionary, "Filter", PDFObject::createName("FlateDecode"));
    return builder.addObject(streamObject(dictionary, qCompress(data, 9).mid(4)));
}
} // namespace
void RedactionFontCvtUsage::record(pdf::PDFObjectReference generated,
                                   const QByteArray& sourceProgram, const QSet<quint16>& references)
{
    if (!generated.isValid() || files.contains(generated) || files.size() >= 100000 ||
        sourceProgram.size() > 16 * 1024 * 1024 || references.size() > 1000000 - referenceCount)
        invalid();
    referenceCount += references.size();
    files.insert(generated,
                 {QCryptographicHash::hash(sourceProgram, QCryptographicHash::Sha256), references});
}
void RedactionFontCvtUsage::validate(const std::set<pdf::PDFObjectReference>& reachable,
                                     const std::function<bool()>& cancelled) const
{
    QMap<QByteArray, QSet<quint16>> references;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
        if (reachable.contains(it.key()))
            references[it->sourceHash].unite(it->references);
    }
    for (const auto& retained : references)
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
        if (retained.isEmpty())
            invalid(); // Removed-only and unreferenced CVT data never grant a witness.
    }
}
PDFObject rebuildRedactionFont(const FontDescriptor& source, const QVector<RedactionGlyph>& glyphs,
                               PDFDocumentBuilder& builder, const std::function<bool()>& cancelled,
                               RedactionFontCvtUsage* cvtUsage, RedactionFontBounds bounds,
                               const QVector<RedactionCodeRoute>& sourceCodes)
{
    if (source.fontFile2.isEmpty() || glyphs.isEmpty() || glyphs.size() > 8191 ||
        sourceCodes.size() > 256)
        invalid();
    const auto program = subset(source.fontFile2, glyphs, cancelled, cvtUsage != nullptr, bounds);
    PDFDictionary file;
    set(file, "Length1", PDFObject::createInteger(program.bytes.size()));
    PDFDictionary descriptor;
    set(descriptor, "Type", PDFObject::createName("FontDescriptor"));
    set(descriptor, "FontName", PDFObject::createName("TatsujinRedacted"));
    set(descriptor, "FontBBox", rectObject(source.boundingBox));
    set(descriptor, "Flags", PDFObject::createInteger(4));
    for (const auto& [name, value] :
         std::vector<std::pair<const char*, double>>{{"Ascent", source.ascent},
                                                     {"Descent", source.descent},
                                                     {"CapHeight", source.capHeight},
                                                     {"ItalicAngle", source.italicAngle},
                                                     {"StemV", source.stemV}})
        set(descriptor, name, number(value));
    const auto fontFile = compressed(builder, file, program.bytes);
    if (program.hasCvt && cvtUsage)
        cvtUsage->record(fontFile, source.fontFile2, program.cvtReferences);
    set(descriptor, "FontFile2", PDFObject::createReference(fontFile));
    QByteArray mapping(2, '\0');
    QByteArray cmap =
        "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n"
        "/CMapName /TatsujinRedacted def /CMapType 2 def\n"
        "1 begincodespacerange <0000> <FFFF> endcodespacerange\n";
    std::vector<PDFObject> widths{number(0)};
    int cid = 0;
    for (const auto& glyph : glyphs)
    {
        append16(mapping, program.gids[glyph.gid]);
        widths.push_back(number(glyph.width));
        cmap += "1 beginbfchar\n<" + QByteArray::number(++cid, 16).rightJustified(4, '0') + "> <" +
                QByteArray::number(glyph.character.unicode(), 16).rightJustified(4, '0') +
                ">\nendbfchar\n";
    }
    cmap += "endcmap CMapName currentdict /CMap defineresource pop end end\n";
    PDFObject encoding = PDFObject::createName("Identity-H");
    if (!sourceCodes.isEmpty())
    {
        // Exact one-code ranges must fit the same bounded decoder on reedit.
        const QByteArray header =
            "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n";
        QByteArray codeSpaces;
        QByteArray cidRoutes, unicodeRoutes;
        QSet<QByteArray> used;
        QSet<quint16> usedCids;
        for (const auto& route : sourceCodes)
        {
            if (cancelled && cancelled())
                fail("墨消しを中止しました。文書は変更していません。");
            if ((route.code.size() != 1 && route.code.size() != 2) || used.contains(route.code) ||
                route.subsetCid <= 0 || route.subsetCid > glyphs.size() ||
                route.character != glyphs[route.subsetCid - 1].character)
                invalid();
            used.insert(route.code);
            usedCids.insert(quint16(route.subsetCid));
            const auto code = "<" + route.code.toHex() + ">";
            codeSpaces += code + " " + code + "\n";
            cidRoutes += "1 begincidchar\n" + code + " " + QByteArray::number(route.subsetCid) +
                         "\nendcidchar\n";
            unicodeRoutes +=
                "1 beginbfchar\n" + code + " <" +
                QByteArray::number(route.character.unicode(), 16).rightJustified(4, '0') +
                ">\nendbfchar\n";
        }
        if (usedCids.size() != glyphs.size())
            invalid();
        const auto spaces = QByteArray::number(sourceCodes.size()) + " begincodespacerange\n" +
                            codeSpaces + "endcodespacerange\n";
        const QByteArray footer = "endcmap CMapName currentdict /CMap defineresource pop end end\n";
        const auto rawEncoding =
            header + "/CMapName /TatsujinRetainedCodes def /CMapType 1 def /WMode 0 def\n" +
            spaces + cidRoutes + footer;
        cmap = header + "/CMapName /TatsujinRedacted def /CMapType 2 def /WMode 0 def\n" + spaces +
               unicodeRoutes + footer;
        encoding = PDFObject::createReference(compressed(builder, {}, rawEncoding));
    }
    PDFDictionary cidFont;
    set(cidFont, "Type", PDFObject::createName("Font"));
    set(cidFont, "Subtype", PDFObject::createName("CIDFontType2"));
    set(cidFont, "BaseFont", PDFObject::createName("TatsujinRedacted"));
    PDFDictionary system;
    set(system, "Registry", PDFObject::createString("Adobe"));
    set(system, "Ordering", PDFObject::createString("Identity"));
    set(system, "Supplement", PDFObject::createInteger(0));
    set(cidFont, "CIDSystemInfo", dictObject(system));
    set(cidFont, "FontDescriptor",
        PDFObject::createReference(builder.addObject(dictObject(descriptor))));
    set(cidFont, "W", arrObject({PDFObject::createInteger(0), arrObject(std::move(widths))}));
    set(cidFont, "CIDToGIDMap", PDFObject::createReference(compressed(builder, {}, mapping)));
    PDFDictionary font;
    set(font, "Type", PDFObject::createName("Font"));
    set(font, "Subtype", PDFObject::createName("Type0"));
    set(font, "BaseFont", PDFObject::createName("TatsujinRedacted"));
    set(font, "Encoding", std::move(encoding));
    set(font, "DescendantFonts",
        arrObject({PDFObject::createReference(builder.addObject(dictObject(cidFont)))}));
    set(font, "ToUnicode", PDFObject::createReference(compressed(builder, {}, cmap)));
    return dictObject(font);
}
} // namespace tatsu
