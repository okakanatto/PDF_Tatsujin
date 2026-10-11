#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct OptimizationResult
{
    PDFDocument document;
    qint64 beforeBytes = 0, afterBytes = 0;
    int sharedStreams = 0, removedObjects = 0;
    bool smaller() const
    {
        return afterBytes < beforeBytes;
    }
};
OptimizationResult optimizePdf(const PDFDocument& document,
                               const std::function<bool()>& cancelled = {},
                               const std::function<void(QString)>& progress = {});
} // namespace tatsu
