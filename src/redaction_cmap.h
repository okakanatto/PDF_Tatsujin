#pragma once
#include "document.h"
#include "pdffont.h"

namespace tatsu
{
// Embedded horizontal CMaps with unambiguous one/two-byte source codes and
// one BMP Unicode value per code. No PostScript or inherited maps execute.
class RedactionCMap
{
public:
    RedactionCMap(const PDFDocument& document, const pdf::PDFDictionary& font,
                  const std::function<bool()>& cancelled);
    struct Code
    {
        pdf::CID cid;
        QChar character;
    };
    QVector<Code> decode(const QByteArray& bytes) const;
    void validate(const QByteArray& bytes) const;
    bool hasSingleByteCodes() const
    {
        return singleByteCodes;
    }

private:
    Code nextCode(const QByteArray& bytes, qsizetype& at) const;
    QMap<QPair<quint8, quint16>, Code> codes;
    QMap<quint8, quint8> codeLengths;
    bool singleByteCodes = false;
    std::function<bool()> cancelled;
};
} // namespace tatsu
