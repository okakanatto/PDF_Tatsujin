#include "redaction_hint_program.h"
#include "document.h"
#include <QtEndian>

namespace tatsu
{
namespace
{
void unsupported()
{
    fail("この字体のヒント命令の描画と安全性を確認できません。墨消し候補は保存していません。");
}
quint16 readOperand(const QByteArray& program, qsizetype& at)
{
    if (at >= program.size())
        unsupported();
    const auto push = quint8(program[at++]);
    if (push == 0xb0 && at < program.size())
        return quint8(program[at++]);
    if (push == 0xb8 && at <= program.size() - 2)
    {
        const auto value = qFromBigEndian<qint16>(program.constData() + at);
        at += 2;
        if (value > 255)
            return quint16(value); // Canonical signed PUSHW only, never a negative index.
    }
    unsupported();
    return 0;
}
void appendOperand(QByteArray& program, quint16 value)
{
    if (value <= 255)
    {
        program.append(char(0xb0));
        program.append(char(value));
    }
    else if (value <= 32767)
    {
        program.append(char(0xb8));
        program.append(char(value >> 8));
        program.append(char(value));
    }
    else
        unsupported();
}
} // namespace
void validateRedactionHintTables(const QMap<QByteArray, QByteArray>& tables)
{
    // Dropout configuration from the frozen inputs. Preserve its exact meaning;
    // no arbitrary initial program, CVT writes, storage or function data is accepted.
    const auto prep = tables.value("prep");
    if (!prep.isEmpty() && prep != QByteArray::fromHex("b801ff85b0048d"))
        unsupported();
    if (tables.value("cvt ").size() % 2)
        unsupported(); // CVT consists of signed FWORDs, never an arbitrary byte tail.
    for (const auto tag : {"fpgm", "cvar", "gasp", "hdmx", "LTSH", "VDMX", "EBDT", "EBLC", "EBSC",
                           "CBDT", "CBLC", "sbix"})
        if (!tables.value(tag).isEmpty())
            unsupported();
}
RedactionGlyphHints validateRedactionGlyphHints(const QByteArray& program, quint32 points,
                                                quint32 cvtCount)
{
    RedactionGlyphHints result;
    if (program.isEmpty())
        return result;
    if (!points || program.size() > 4096)
        unsupported();
    qsizetype at = 0;
    int previousAxis = -1;
    while (at < program.size())
    {
        const int axis = quint8(program[at++]);
        if (axis > 1 || axis <= previousAxis)
            unsupported();
        previousAxis = axis;
        qint32 previousPoint = -1;
        while (at < program.size() && quint8(program[at]) != 0x30 + axis)
        {
            const auto point = readOperand(program, at);
            if (point >= points || qint32(point) <= previousPoint || at >= program.size())
                unsupported();
            if (quint8(program[at]) == 0x2f)
            {
                ++at;
                result.maxStackElements = qMax(result.maxStackElements, quint16(1));
            }
            else
            {
                const auto operandAt = at;
                const auto index = readOperand(program, at);
                if (index >= cvtCount || at >= program.size() || quint8(program[at++]) != 0x3f)
                    unsupported();
                result.cvtOperands.insert(operandAt, index);
                result.maxStackElements = 2; // MIAP pops the CVT index, then the real point.
            }
            previousPoint = qint32(point);
        }
        if (previousPoint < 0 || at >= program.size())
            unsupported();
        ++at; // Matching IUP closes the axis with an empty stack.
    }
    result.instructionBytes = quint16(program.size());
    return result;
}
QByteArray rewriteRedactionGlyphHints(const QByteArray& program, const RedactionGlyphHints& hints,
                                      const QMap<quint16, quint16>& cvtIndices)
{
    if (program.size() != hints.instructionBytes || program.size() > 4096)
        unsupported();
    QByteArray result;
    qsizetype copied = 0;
    for (auto it = hints.cvtOperands.cbegin(); it != hints.cvtOperands.cend(); ++it)
    {
        const auto at = it.key();
        if (at < copied || at >= program.size() || !cvtIndices.contains(it.value()))
            unsupported();
        auto end = at;
        if (readOperand(program, end) != it.value() || cvtIndices[it.value()] > it.value())
            unsupported();
        result += program.mid(copied, at - copied);
        appendOperand(result, cvtIndices[it.value()]);
        copied = end;
    }
    result += program.mid(copied);
    if (result.size() > program.size())
        unsupported(); // Ascending dense indices can only preserve or shorten a PUSH.
    return result;
}
} // namespace tatsu
