#include "memory_diagnostics.h"
#include "page_previews.h"
#include "window.h"
#include <QPixmapCache>
#include <cstdio>

namespace
{
void require(bool condition, const QString& message)
{
    if (!condition)
        tatsu::fail(message);
}
void ready(tatsu::Canvas* canvas)
{
    QElapsedTimer timer;
    timer.start();
    while (true)
    {
        QCoreApplication::processEvents();
        const auto pages = canvas->visiblePages();
        if (!pages.isEmpty() && std::all_of(pages.begin(), pages.end(),
                                            [&](int page) { return canvas->pageReady(page); }))
            return;
        require(timer.elapsed() < 30000, "Soak: page readiness timed out");
        QThread::msleep(1);
    }
}
void idle(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
    {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try
    {
        const auto args = app.arguments();
        require(args.size() == 4, "Expected fixtures, new output and duration seconds");
        bool valid = false;
        const int seconds = args[3].toInt(&valid);
        require(valid && seconds >= 10 && seconds <= 7200, "Invalid soak duration");
        const auto fixtures = QFileInfo(args[1]).absoluteFilePath();
        const auto output = QFileInfo(args[2]).absoluteFilePath();
        require(!QFileInfo::exists(output) && QDir().mkpath(output), "Output already exists");
        app.setFont(QFont(tatsu::signatureFont(), 10));
        const auto mode = qEnvironmentVariable("TATSU_SOAK_MODE", "mixed");
        require(QStringList{"mixed", "empty", "digital", "image", "reuse"}.contains(mode),
                "Unsupported soak mode");
        std::unique_ptr<tatsu::Window> reused;
        if (mode == "reuse")
            reused = std::make_unique<tatsu::Window>();
        QFile observations(output + "/cycles.jsonl");
        require(observations.open(QIODevice::WriteOnly), "Cannot write observations");
        QElapsedTimer elapsed;
        elapsed.start();
        int cycle = 0, pagesVisited = 0;
        while (elapsed.elapsed() < seconds * 1000LL)
        {
            const auto name = mode == "empty" ? QString()
                              : mode == "image" || (mode != "digital" && cycle % 2)
                                  ? QString("D10-image-50.pdf")
                                  : QString("D10-digital-100.pdf");
            const auto source = fixtures + "/" + name;
            const auto sourceHash = name.isEmpty() ? QByteArray() : tatsu::fileHash(source);
            int pages = 0;
            qint64 maxText = 0, maxPreviews = 0;
            {
                auto owned =
                    reused ? std::unique_ptr<tatsu::Window>() : std::make_unique<tatsu::Window>();
                auto& window = reused ? *reused : *owned;
                window.resize(1280, 850);
                window.show();
                if (name.isEmpty())
                {
                    idle(50);
                    require(!window.doc.loaded() && !window.doc.dirty(),
                            "Empty window has a document");
                    window.close();
                }
                else
                {
                    window.openFile(source);
                    ready(window.canvas);
                    const auto snapshot = tatsu::encodePdf(window.doc.pdf());
                    const auto revision = window.doc.revision;
                    const auto history = window.doc.history.size();
                    auto previews = static_cast<tatsu::PagePreviews*>(window.pages);
                    pages = window.doc.pages();
                    for (int sequence = 0; sequence < pages; ++sequence)
                    {
                        const int page = cycle % 4 < 2 ? sequence : pages - sequence - 1;
                        window.canvas->goToPage(page);
                        ready(window.canvas);
                        window.canvas->viewport()->repaint();
                        idle(100);
                        maxText = qMax(maxText, window.canvas->selectionCacheBytes());
                        maxPreviews = qMax(maxPreviews, previews->cacheBytes());
                        ++pagesVisited;
                    }
                    require(!window.doc.dirty() && window.doc.cursor == 0 &&
                                window.doc.revision == revision &&
                                window.doc.history.size() == history &&
                                tatsu::encodePdf(window.doc.pdf()) == snapshot,
                            "Reading changed document or Undo state");
                    require(maxText <= 16 * 1024 * 1024 && maxPreviews <= 8 * 1024 * 1024,
                            "Existing selection/preview cache limit exceeded");
                    window.close();
                }
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            idle(300);
            if (!name.isEmpty())
                require(tatsu::fileHash(source) == sourceHash, "Source PDF changed");
            QJsonObject observation{{"cycle", ++cycle},
                                    {"file", name},
                                    {"pages", pages},
                                    {"at_ms", elapsed.elapsed()},
                                    {"idle_unix_ms", QDateTime::currentMSecsSinceEpoch()},
                                    {"max_text_cache_bytes", maxText},
                                    {"max_preview_cache_bytes", maxPreviews},
                                    {"document_and_Undo_unchanged", true}};
            if (qEnvironmentVariableIsSet("TATSU_HEAP_DIAGNOSTICS"))
                observation.insert("heap_diagnostic", tatsu::diagnostics::defaultHeap());
            if (qEnvironmentVariableIsSet("TATSU_MEMORY_DIAGNOSTICS"))
                observation.insert("memory_diagnostic", tatsu::diagnostics::memorySnapshot());
            observations.write(QJsonDocument(observation).toJson(QJsonDocument::Compact) + "\n");
            observations.flush();
        }
        QJsonObject result{{"status", "PASS"},
                           {"requested_seconds", seconds},
                           {"elapsed_ms", elapsed.elapsed()},
                           {"cycles", cycle},
                           {"pages_visited", pagesVisited},
                           {"mode", mode},
                           {"scope",
                            "Product Window lifecycle with explicitly recorded mode; "
                            "frozen digital/image PDFs where used, offscreen, no FPS claim"}};
        reused.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        idle(300);
        if (qEnvironmentVariableIsSet("TATSU_PIXMAP_DIAGNOSTICS"))
        {
            result.insert("before_pixmap_clear", tatsu::diagnostics::memorySnapshot());
            result.insert("before_pixmap_heap", tatsu::diagnostics::defaultHeap());
            result.insert("pixmap_cache_limit_KiB", QPixmapCache::cacheLimit());
            QPixmapCache::clear();
            idle(300);
            result.insert("after_pixmap_clear", tatsu::diagnostics::memorySnapshot());
            result.insert("after_pixmap_heap", tatsu::diagnostics::defaultHeap());
        }
        if (qEnvironmentVariableIsSet("TATSU_TRIM_DIAGNOSTICS"))
        {
            result.insert("before_trim", tatsu::diagnostics::memorySnapshot());
            result.insert("heap_trim", tatsu::diagnostics::trimHeapCaches());
            idle(300);
            result.insert("after_trim", tatsu::diagnostics::memorySnapshot());
        }
        QFile report(output + "/result.json");
        require(report.open(QIODevice::WriteOnly), "Cannot write result");
        report.write(QJsonDocument(result).toJson());
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
