#include "pdf_viewer_adapter.h"
#include "pdfcompiler.h"
#include "pdfdrawspacecontroller.h"
#include "pdfpainter.h"
#include <algorithm>

namespace tatsu
{
void prepareReadingPages(pdf::PDFDrawWidgetProxy* proxy,
                         const std::vector<pdf::PDFInteger>& visible, int direction)
{
    if (visible.empty() || !proxy->getDocument())
        return;
    auto compiler = proxy->getCompiler();
    const auto count = pdf::PDFInteger(proxy->getDocument()->getCatalog()->getPageCount());
    auto keep = visible;
    const auto [first, last] = std::minmax_element(visible.begin(), visible.end());
    if (*first > 0)
        keep.push_back(*first - 1);
    if (*last + 1 < count)
        keep.push_back(*last + 1);
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    compiler->smartClearCache(0, keep);
    bool ready = true;
    qint64 visibleBytes = 0;
    for (const auto page : visible)
    {
        const auto compiled = compiler->getCompiledPage(page, true);
        ready &= compiled && compiled->isValid();
        if (compiled)
            visibleBytes += compiled->getMemoryConsumptionEstimate();
    }
    // Leave headroom for the visible content and one decode in the existing cache.
    // Large visible pages keep the original renderer; they need no speculative work.
    if (ready && visibleBytes <= 64 * 1024 * 1024)
    {
        const auto adjacent = direction < 0 ? *first - 1 : *last + 1;
        if (adjacent >= 0 && adjacent < count)
            compiler->getCompiledPage(adjacent, true);
    }
}
} // namespace tatsu
