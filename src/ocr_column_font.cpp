#include "ocr_column_font.h"
#include "document.h"
#include <QtEndian>
#include <algorithm>

namespace tatsu
{
namespace
{
struct Table
{
    qsizetype directory, offset, length;
};
quint32 checksum(const QByteArray& bytes, qsizetype start, qsizetype length)
{
    quint32 sum = 0;
    for (qsizetype i = 0; i < length; i += 4)
    {
        quint32 word = 0;
        for (int j = 0; j < 4; ++j)
            word = (word << 8) | (i + j < length ? quint8(bytes[start + i + j]) : 0);
        sum += word;
    }
    return sum;
}
} // namespace
QByteArray columnOcrFont(QByteArray font)
{
    const auto invalid = [] { fail("縦書きOCRの埋込み字体を安全に構成できません。"); };
    if (font.size() < 12 || font.size() > 16 * 1024 * 1024 ||
        qFromBigEndian<quint32>(font.constData()) != 0x00010000)
        invalid();
    const auto count = qFromBigEndian<quint16>(font.constData() + 4);
    if (!count || count > 256 || font.size() < 12 + 16 * count)
        invalid();
    QMap<QByteArray, Table> tables;
    QVector<QPair<qsizetype, qsizetype>> ranges;
    for (int i = 0; i < count; ++i)
    {
        const qsizetype entry = 12 + i * 16;
        const auto tag = font.mid(entry, 4);
        const qsizetype offset = qFromBigEndian<quint32>(font.constData() + entry + 8);
        const qsizetype length = qFromBigEndian<quint32>(font.constData() + entry + 12);
        if (tables.contains(tag) || tag == "DSIG" || offset < 12 + 16 * count || offset % 4 ||
            offset > font.size() || length > font.size() - offset)
            invalid();
        tables.insert(tag, {entry, offset, length});
        if (length)
            ranges.append({offset, offset + length});
    }
    std::sort(ranges.begin(), ranges.end());
    for (int i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second)
            invalid();
    for (const auto tag : {"head", "hhea", "hmtx", "maxp", "glyf", "loca"})
        if (!tables.contains(tag))
            invalid();
    const auto head = tables["head"], hhea = tables["hhea"], hmtx = tables["hmtx"],
               maxp = tables["maxp"];
    if (head.length < 54 || hhea.length < 36 || maxp.length < 6)
        invalid();
    const auto units = qFromBigEndian<quint16>(font.constData() + head.offset + 18);
    const auto glyphs = qFromBigEndian<quint16>(font.constData() + maxp.offset + 4);
    const auto metrics = qFromBigEndian<quint16>(font.constData() + hhea.offset + 34);
    if (units < 16 || units > 16384 || !glyphs || !metrics || metrics > glyphs ||
        hmtx.length < 4 * metrics + 2 * (glyphs - metrics))
        invalid();
    // The PDF W array uses the same 2em advance. Updating the font program as
    // well avoids stretching outlines in readers that reconcile these metrics.
    for (int i = 0; i < metrics; ++i)
        qToBigEndian<quint16>(2 * units, font.data() + hmtx.offset + i * 4);
    qToBigEndian<quint16>(2 * units, font.data() + hhea.offset + 10);
    qToBigEndian<quint32>(0, font.data() + head.offset + 8);
    for (const auto& table : tables)
        qToBigEndian<quint32>(checksum(font, table.offset, table.length),
                              font.data() + table.directory + 4);
    qToBigEndian<quint32>(0xB1B0AFBAu - checksum(font, 0, font.size()),
                          font.data() + head.offset + 8);
    return font;
}
} // namespace tatsu
