#pragma once
#include <QByteArray>
#include <QVector>
#include <array>

namespace tatsu
{
struct RedactionGlyphMetrics
{
    quint32 points = 0, contours = 0;
    quint16 componentCount = 0, componentDepth = 0;
    quint16 instructionBytes = 0, maxStackElements = 0;
    bool composite = false;
    std::array<qint16, 4> bounds{};
    quint16 advance = 0;
    qint16 bearing = 0;
};
struct RedactionSfntHeaders
{
    QByteArray head, hhea, maxp;
};
RedactionSfntHeaders rebuildRedactionSfntHeaders(const QByteArray& originalHead,
                                                 const QByteArray& originalHhea,
                                                 const QVector<RedactionGlyphMetrics>& glyphs);
} // namespace tatsu
