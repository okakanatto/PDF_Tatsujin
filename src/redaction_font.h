#pragma once
#include "document.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include <QMap>
#include <QSet>
#include <functional>
#include <set>

namespace tatsu
{
struct RedactionGlyph
{
    quint16 gid;
    QChar character;
    double width;
};
enum class RedactionFontBounds
{
    RetainedGlyphs,
    EmbeddedCMapSource
};
// Candidate-owned CVT witnesses. Only final reachable generated font files
// contribute retained references; overwritten intermediate subsets do not.
class RedactionFontCvtUsage
{
public:
    void record(pdf::PDFObjectReference generated, const QByteArray& sourceProgram,
                const QSet<quint16>& references);
    void validate(const std::set<pdf::PDFObjectReference>& reachable,
                  const std::function<bool()>& cancelled) const;
    bool empty() const
    {
        return files.isEmpty();
    }

private:
    struct Font
    {
        QByteArray sourceHash;
        QSet<quint16> references;
    };
    QMap<pdf::PDFObjectReference, Font> files;
    qsizetype referenceCount = 0;
};
// Rebuild a bounded TrueType program and PDF mappings from surviving glyphs.
// No original cmap, names, unused glyphs or private tables enter the candidate.
pdf::PDFObject
rebuildRedactionFont(const pdf::FontDescriptor& descriptor, const QVector<RedactionGlyph>& glyphs,
                     pdf::PDFDocumentBuilder& builder, const std::function<bool()>& cancelled,
                     RedactionFontCvtUsage* cvtUsage = nullptr,
                     RedactionFontBounds bounds = RedactionFontBounds::RetainedGlyphs);
} // namespace tatsu
