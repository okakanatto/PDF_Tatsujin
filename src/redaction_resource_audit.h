#pragma once
#include "document.h"
#include <functional>
#include <set>

namespace tatsu
{
class RedactionFontCvtUsage;
// The same bounded decoder is used by use-site matching and final auditing.
QByteArray redactionStreamFingerprint(const pdf::PDFObjectStorage& storage,
                                      const pdf::PDFStream* stream, qint64& total,
                                      const std::function<bool()>& cancelled = {});
// Inspect only the reachable sanitized candidate, before removing its orphans.
// Reference identity catches shared resources; decoded hashes catch independent
// copies of removed font/content streams within the raw/ASCII85/Flate profile.
void rejectRetainedRedactionResources(const PDFDocument& source,
                                      const pdf::PDFObjectStorage& candidate,
                                      const std::set<pdf::PDFObjectReference>& images,
                                      const std::set<pdf::PDFObjectReference>& removedDependencies,
                                      const QVector<pdf::PDFObject>& directFonts,
                                      const std::function<bool()>& cancelled = {},
                                      const RedactionFontCvtUsage* cvtUsage = nullptr);
} // namespace tatsu
