#include "redaction_sfnt_metrics.h"
#include "document.h"
#include <QtEndian>
#include <algorithm>
#include <limits>

namespace tatsu
{
namespace
{
quint16 read16(const QByteArray& bytes, int at)
{
    return qFromBigEndian<quint16>(bytes.constData() + at);
}
void put16(QByteArray& bytes, int at, quint16 value)
{
    qToBigEndian<quint16>(value, bytes.data() + at);
}
void put32(QByteArray& bytes, int at, quint32 value)
{
    qToBigEndian<quint32>(value, bytes.data() + at);
}
void signed16(QByteArray& bytes, int at, qint32 value)
{
    if (value < std::numeric_limits<qint16>::min() || value > std::numeric_limits<qint16>::max())
        fail("保持字体の集計値がTrueTypeの範囲を超えています。");
    put16(bytes, at, quint16(qint16(value)));
}
} // namespace
RedactionSfntHeaders rebuildRedactionSfntHeaders(const QByteArray& originalHead,
                                                 const QByteArray& originalHhea,
                                                 const QVector<RedactionGlyphMetrics>& glyphs)
{
    if (originalHead.size() != 54 || originalHhea.size() != 36 || glyphs.isEmpty() ||
        glyphs.size() > 8192)
        fail("保持字体のヘッダを安全に再構築できません。");
    const auto units = read16(originalHead, 18);
    if (units < 16 || units > 16384)
        fail("保持字体の座標倍率が不正です。");
    RedactionSfntHeaders output{QByteArray(54, '\0'), QByteArray(36, '\0'), QByteArray(32, '\0')};
    put32(output.head, 0, 0x10000);
    put32(output.head, 4, 0x10000); // New program, no manufacturer's private revision.
    put32(output.head, 12, 0x5f0f3cf5);
    // Baseline, side-bearing and fractional ppem behavior are rendering inputs.
    put16(output.head, 16, read16(originalHead, 16) & 0x000b);
    put16(output.head, 18, units);
    put16(output.head, 44, read16(originalHead, 44) & 0x007f);
    put16(output.head, 46, 8);
    put16(output.head, 48, 2);
    put16(output.head, 50, 1);
    put32(output.hhea, 0, 0x10000);
    // Preserve the original layout metrics; every other field is regenerated.
    output.hhea.replace(4, 6, originalHhea.mid(4, 6));
    put16(output.hhea, 18, 1);
    put16(output.hhea, 34, glyphs.size());
    put32(output.maxp, 0, 0x10000);
    put16(output.maxp, 4, glyphs.size());
    put16(output.maxp, 14, 1); // No retained instruction uses a twilight zone.
    std::array<qint32, 4> bounds{};
    qint32 minBearing = 0, minRight = 0, maxExtent = 0;
    quint16 maxAdvance = 0;
    bool first = true;
    for (const auto& glyph : glyphs)
    {
        if (glyph.instructionBytes)
        {
            put16(output.maxp, 24, qMax(read16(output.maxp, 24), glyph.maxStackElements));
            put16(output.maxp, 26, qMax(read16(output.maxp, 26), glyph.instructionBytes));
        }
        if (glyph.points > 65535 || glyph.contours > 65535)
            fail("保持字体の輪郭がTrueTypeの範囲を超えています。");
        const int pointField = glyph.composite ? 10 : 6;
        const int contourField = glyph.composite ? 12 : 8;
        put16(output.maxp, pointField,
              qMax(read16(output.maxp, pointField), quint16(glyph.points)));
        put16(output.maxp, contourField,
              qMax(read16(output.maxp, contourField), quint16(glyph.contours)));
        put16(output.maxp, 28, qMax(read16(output.maxp, 28), glyph.componentCount));
        put16(output.maxp, 30, qMax(read16(output.maxp, 30), glyph.componentDepth));
        maxAdvance = qMax(maxAdvance, glyph.advance);
        if (!glyph.contours)
            continue;
        const qint32 width = qint32(glyph.bounds[2]) - glyph.bounds[0];
        const qint32 extent = glyph.bearing + width, right = qint32(glyph.advance) - extent;
        if (first)
        {
            std::copy(glyph.bounds.begin(), glyph.bounds.end(), bounds.begin());
            minBearing = glyph.bearing;
            minRight = right;
            maxExtent = extent;
            first = false;
        }
        else
        {
            bounds[0] = qMin(bounds[0], qint32(glyph.bounds[0]));
            bounds[1] = qMin(bounds[1], qint32(glyph.bounds[1]));
            bounds[2] = qMax(bounds[2], qint32(glyph.bounds[2]));
            bounds[3] = qMax(bounds[3], qint32(glyph.bounds[3]));
            minBearing = qMin(minBearing, qint32(glyph.bearing));
            minRight = qMin(minRight, right);
            maxExtent = qMax(maxExtent, extent);
        }
    }
    for (int i = 0; i < 4; ++i)
        signed16(output.head, 36 + 2 * i, bounds[i]);
    put16(output.hhea, 10, maxAdvance);
    signed16(output.hhea, 12, minBearing);
    signed16(output.hhea, 14, minRight);
    signed16(output.hhea, 16, maxExtent);
    return output;
}
} // namespace tatsu
