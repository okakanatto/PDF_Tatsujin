#pragma once
#include "document.h"
#include <functional>
#include <map>
#include <set>

namespace tatsu
{
// Match known font uses, including independent copies, without granting an
// exemption to private aliases. Final reachability auditing remains mandatory.
class RedactionResourceUsage
{
public:
    RedactionResourceUsage(const PDFDocument& source, std::set<pdf::PDFObjectReference> removed,
                           std::set<pdf::PDFObjectReference> forbiddenForms,
                           QMap<pdf::PDFObjectReference, QPainterPath> formRegions,
                           QVector<pdf::PDFObject> directFonts, std::function<bool()> cancelled);
    bool affectedFont(const pdf::PDFObject& declaration) const;
    bool affectedResources(const pdf::PDFObject& resources, int depth = 0) const;
    bool forbiddenForm(const pdf::PDFObject& form) const;
    void validateFormSelection(const pdf::PDFObject& form, const QPainterPath& regions) const;

private:
    void stop() const;
    const PDFDocument& source;
    std::set<pdf::PDFObjectReference> removed, forbiddenForms;
    std::set<QByteArray> fingerprints;
    QVector<pdf::PDFObject> directFonts;
    std::set<QByteArray> forbiddenFormFingerprints;
    QMap<QByteArray, QPainterPath> formRegionsByHash;
    std::function<bool()> cancelled;
    mutable std::map<pdf::PDFObjectReference, bool> fonts;
    mutable std::map<pdf::PDFObjectReference, QByteArray> hashes;
    mutable qint64 decodedTotal = 0;
    mutable size_t resourceVisits = 0;
};
} // namespace tatsu
