#pragma once
#include <QByteArray>
#include <QMap>

namespace tatsu
{
// A closed grammar whose operands are consumed real outline point numbers
// and read-only CVT indices. Only validated CVT operands may be rewritten.
// Unknown executable data must fail before a candidate can be published.
struct RedactionGlyphHints
{
    QMap<qsizetype, quint16> cvtOperands;
    quint16 instructionBytes = 0, maxStackElements = 0;
};
void validateRedactionHintTables(const QMap<QByteArray, QByteArray>& tables);
RedactionGlyphHints validateRedactionGlyphHints(const QByteArray& program, quint32 points,
                                                quint32 cvtCount);
QByteArray rewriteRedactionGlyphHints(const QByteArray& program, const RedactionGlyphHints& hints,
                                      const QMap<quint16, quint16>& cvtIndices);
} // namespace tatsu
