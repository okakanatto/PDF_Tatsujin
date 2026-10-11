#pragma once
#include "document.h"
#include <functional>
#include <set>

namespace tatsu
{
// Inspect only the reachable sanitized candidate, before removing its orphans.
// Reference identity catches shared resources; decoded hashes catch independent
// copies of removed font/content streams within the raw/ASCII85/Flate profile.
void rejectRetainedRedactionResources(const PDFDocument& source,
                                      const pdf::PDFObjectStorage& candidate,
                                      const std::set<pdf::PDFObjectReference>& images,
                                      const std::set<pdf::PDFObjectReference>& removedDependencies,
                                      const std::function<bool()>& cancelled = {});
} // namespace tatsu
