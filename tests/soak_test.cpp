#include "page_previews.h"
#include "window.h"
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
        QFile observations(output + "/cycles.jsonl");
        require(observations.open(QIODevice::WriteOnly), "Cannot write observations");
        QElapsedTimer elapsed;
        elapsed.start();
        int cycle = 0, pagesVisited = 0;
        while (elapsed.elapsed() < seconds * 1000LL)
        {
            const auto name = cycle % 2 ? "D10-image-50.pdf" : "D10-digital-100.pdf";
            const auto source = fixtures + "/" + name;
            const auto sourceHash = tatsu::fileHash(source);
            int pages = 0;
            qint64 maxText = 0, maxPreviews = 0;
            {
                tatsu::Window window;
                window.resize(1280, 850);
                window.show();
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
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            idle(300);
            require(tatsu::fileHash(source) == sourceHash, "Source PDF changed");
            const QJsonObject observation{{"cycle", ++cycle},
                                          {"file", name},
                                          {"pages", pages},
                                          {"at_ms", elapsed.elapsed()},
                                          {"idle_unix_ms", QDateTime::currentMSecsSinceEpoch()},
                                          {"max_text_cache_bytes", maxText},
                                          {"max_preview_cache_bytes", maxPreviews},
                                          {"document_and_Undo_unchanged", true}};
            observations.write(QJsonDocument(observation).toJson(QJsonDocument::Compact) + "\n");
            observations.flush();
        }
        const QJsonObject result{{"status", "PASS"},
                                 {"requested_seconds", seconds},
                                 {"elapsed_ms", elapsed.elapsed()},
                                 {"cycles", cycle},
                                 {"pages_visited", pagesVisited},
                                 {"scope", "Repeated product Window open/read/close, alternating "
                                           "frozen digital/image PDFs; offscreen, no FPS claim"}};
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
