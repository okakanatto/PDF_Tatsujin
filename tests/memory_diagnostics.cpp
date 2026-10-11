#include "memory_diagnostics.h"
#include "pdfexecutionpolicy.h"
#include <QApplication>
#include <cstdint>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace tatsu::diagnostics
{
QJsonObject defaultHeap()
{
#ifdef Q_OS_WIN
    // Third-party heaps may disappear concurrently. Never walk those handles.
    const auto heap = GetProcessHeap();
    if (!HeapLock(heap))
        return {{"status", "FAIL"}, {"lock_error", int(GetLastError())}};
    PROCESS_HEAP_ENTRY entry{};
    qint64 busyBytes = 0, freeBytes = 0, busyBlocks = 0;
    while (HeapWalk(heap, &entry))
    {
        if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
        {
            busyBytes += entry.cbData;
            ++busyBlocks;
        }
        else if (!(entry.wFlags & (PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE)))
            freeBytes += entry.cbData;
    }
    const auto error = GetLastError();
    const bool unlocked = HeapUnlock(heap);
    // Allocate/report only after the heap is unlocked.
    return {{"status", error == ERROR_NO_MORE_ITEMS && unlocked ? "PASS" : "FAIL"},
            {"walk_error", int(error)},
            {"heap_unlocked", unlocked},
            {"default_heap_busy_bytes", busyBytes},
            {"default_heap_free_bytes", freeBytes},
            {"default_heap_busy_blocks", busyBlocks},
            {"scope", "Default Win32 heap only; excludes other heaps and virtual mappings"}};
#else
    return {{"status", "unsupported"}};
#endif
}
QJsonObject memorySnapshot()
{
#ifdef Q_OS_WIN
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto cursor = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    qint64 privateCommit = 0, privateReserve = 0, mappedCommit = 0, imageCommit = 0;
    int regions = 0;
    while (cursor <= maximum)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)))
            return {{"status", "FAIL"}, {"query_error", int(GetLastError())}};
        if (region.State == MEM_COMMIT)
        {
            if (region.Type == MEM_PRIVATE)
                privateCommit += region.RegionSize;
            else if (region.Type == MEM_MAPPED)
                mappedCommit += region.RegionSize;
            else if (region.Type == MEM_IMAGE)
                imageCommit += region.RegionSize;
        }
        else if (region.State == MEM_RESERVE && region.Type == MEM_PRIVATE)
            privateReserve += region.RegionSize;
        ++regions;
        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= cursor)
            return {{"status", "FAIL"}, {"query_error", "address overflow"}};
        cursor = next;
    }
    return {{"status", "PASS"},
            {"private_commit_bytes", privateCommit},
            {"private_reserved_bytes", privateReserve},
            {"mapped_commit_bytes", mappedCommit},
            {"image_commit_bytes", imageCommit},
            {"regions", regions},
            {"live_widgets", QApplication::allWidgets().size()},
            {"active_page_tasks",
             pdf::PDFExecutionPolicy::getActiveThreadCount(pdf::PDFExecutionPolicy::Scope::Page)},
            {"active_content_tasks", pdf::PDFExecutionPolicy::getActiveThreadCount(
                                         pdf::PDFExecutionPolicy::Scope::Content)},
            {"scope", "Metadata of own process only; no memory content. Mapped/image copy-on-write "
                      "private pages are not reclassified by VirtualQuery."}};
#else
    return {{"status", "unsupported"}};
#endif
}
QJsonObject trimHeapCaches()
{
#ifdef Q_OS_WIN
    HEAP_OPTIMIZE_RESOURCES_INFORMATION options{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    const bool ok = HeapSetInformation(nullptr, HeapOptimizeResources, &options, sizeof(options));
    return {{"status", ok ? "PASS" : "FAIL"},
            {"error", ok ? 0 : int(GetLastError())},
            {"scope", "Optional diagnostic: optimize own-process LFH caches once after the "
                      "workload. Not part of the product."}};
#else
    return {{"status", "unsupported"}};
#endif
}
} // namespace tatsu::diagnostics
