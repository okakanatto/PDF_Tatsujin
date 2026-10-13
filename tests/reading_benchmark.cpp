#include "page_previews.h"
#include "pdfdrawwidget.h"
#include "window.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>
#include <algorithm>

namespace
{
void require(bool value, const QString& message)
{
    if (!value)
        tatsu::fail(message);
}
void eventsFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
    {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
qint64 ready(tatsu::Canvas* canvas)
{
    QElapsedTimer timer;
    timer.start();
    while (true)
    {
        QCoreApplication::processEvents();
        const auto visible = canvas->visiblePages();
        if (!visible.isEmpty() && std::all_of(visible.begin(), visible.end(),
                                              [&](int page) { return canvas->pageReady(page); }))
            return timer.elapsed();
        require(timer.elapsed() < 30000, "Visible-page rendering timed out");
        QThread::msleep(1);
    }
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3)
        return 2;
    const auto source = QFileInfo(args[1]).absoluteFilePath();
    const auto output = QFileInfo(args[2]).absoluteFilePath();
    try
    {
        app.setFont(QFont(tatsu::signatureFont(), 10));
        require(!QFileInfo::exists(output), "Output directory already exists");
        require(QDir().mkpath(output), "Cannot create output directory");
        const auto sourceHash = tatsu::fileHash(source);
        tatsu::Window window;
        window.resize(1280, 850);
        window.show();
        const auto renderer = qEnvironmentVariable("TATSU_MEASURE_RENDERER", "product");
        const auto views = window.canvas->findChildren<pdf::PDFWidget*>();
        require(views.size() == 1, "Expected one product PDF viewport");
        if (renderer == "blend2d-single")
            views.front()->updateRenderer(pdf::RendererEngine::Blend2D_SingleThread);
        else if (renderer == "blend2d-multi")
            views.front()->updateRenderer(pdf::RendererEngine::Blend2D_MultiThread);
        else
            require(renderer == "product", "Unknown measurement renderer");
        window.openFile(source);
        ready(window.canvas);
        eventsFor(250);
        const auto original = tatsu::encodePdf(window.doc.pdf());
        const auto revision = window.doc.revision;
        const auto historySize = window.doc.history.size();
        auto previews = static_cast<tatsu::PagePreviews*>(window.pages);
        QElapsedTimer elapsed, heartbeat;
        const auto startedUnixMs = QDateTime::currentMSecsSinceEpoch();
        elapsed.start();
        heartbeat.start();
        QJsonArray ticks, laps, steps;
        QSet<int> visited;
        QTimer pulse;
        pulse.setTimerType(Qt::PreciseTimer);
        QObject::connect(&pulse, &QTimer::timeout,
                         [&] {
                             ticks.append(QJsonObject{{"at_ms", elapsed.elapsed()},
                                                      {"interval_ms", heartbeat.restart()}});
                         });
        pulse.start(16);
        qint64 maxTextCache = 0, maxPreviewCache = 0;
        for (int lap = 0; lap < 3; ++lap)
        {
            window.canvas->goToPage(0);
            ready(window.canvas);
            eventsFor(20);
            const auto started = elapsed.elapsed();
            int position = -1;
            int sequence = 0;
            QElapsedTimer stepTimer;
            stepTimer.start();
            while (true)
            {
                auto scroll = window.canvas->verticalScrollBar();
                const auto current = scroll->value();
                require(current != position, "Scroll position did not advance");
                position = current;
                const auto wait = ready(window.canvas);
                QJsonArray visible;
                for (const int page : window.canvas->visiblePages())
                {
                    visited.insert(page);
                    visible.append(page + 1);
                }
                const auto image = window.canvas->viewport()->grab();
                const auto latency = stepTimer.elapsed();
                require(!image.isNull(), "Viewport capture is empty");
                if (sequence == 0 || window.canvas->page == window.doc.pages() / 2 ||
                    current == scroll->maximum())
                    require(image.save(output + QString("/lap-%1-page-%2.png")
                                                    .arg(lap + 1)
                                                    .arg(window.canvas->page + 1)),
                            "Cannot save representative rendering");
                maxTextCache = qMax(maxTextCache, window.canvas->selectionCacheBytes());
                maxPreviewCache = qMax(maxPreviewCache, previews->cacheBytes());
                steps.append(QJsonObject{{"lap", lap + 1},
                                         {"step", sequence++},
                                         {"at_ms", elapsed.elapsed()},
                                         {"scroll", current},
                                         {"visible_pages", visible},
                                         {"ready_ms", latency},
                                         {"render_wait_ms", wait},
                                         {"text_cache_bytes", window.canvas->selectionCacheBytes()},
                                         {"preview_cache_bytes", previews->cacheBytes()}});
                eventsFor(16);
                if (current == scroll->maximum())
                    break;
                stepTimer.restart();
                window.canvas->scrollBy({0, qMax(1, window.canvas->viewport()->height() * 3 / 4)});
            }
            eventsFor(1000);
            require(!window.doc.dirty() && window.doc.cursor == 0 &&
                        window.doc.revision == revision && window.doc.history.size() == historySize,
                    "Reading changed the document or Undo state");
            laps.append(
                QJsonObject{{"lap", lap + 1},
                            {"end_ms", elapsed.elapsed()},
                            {"idle_sample_unix_ms", startedUnixMs + elapsed.elapsed() - 250},
                            {"duration_ms", elapsed.elapsed() - started},
                            {"steps", sequence},
                            {"text_cache_bytes", window.canvas->selectionCacheBytes()},
                            {"preview_cache_bytes", previews->cacheBytes()}});
        }
        pulse.stop();
        require(visited.size() == window.doc.pages(), "Some pages were never displayed");
        require(maxTextCache <= 16 * 1024 * 1024 && maxPreviewCache <= 8 * 1024 * 1024,
                "Existing text or preview cache limit exceeded");
        require(tatsu::encodePdf(window.doc.pdf()) == original &&
                    tatsu::fileHash(source) == sourceHash,
                "Reading changed PDF bytes or the source file");
        QFile report(output + "/run.json");
        require(report.open(QIODevice::WriteOnly), "Cannot write benchmark report");
        const QJsonObject result{{"status", "PASS"},
                                 {"renderer_override", renderer},
                                 {"pages", window.doc.pages()},
                                 {"visited_pages", visited.size()},
                                 {"viewport_width", window.canvas->viewport()->width()},
                                 {"viewport_height", window.canvas->viewport()->height()},
                                 {"elapsed_ms", elapsed.elapsed()},
                                 {"measurement_start_unix_ms", startedUnixMs},
                                 {"laps", laps},
                                 {"steps", steps},
                                 {"timer_ticks", ticks},
                                 {"max_text_cache_bytes", maxTextCache},
                                 {"max_preview_cache_bytes", maxPreviewCache},
                                 {"source_sha256", QString::fromLatin1(sourceHash.toHex())},
                                 {"document_and_Undo_unchanged", true}};
        report.write(QJsonDocument(result).toJson());
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
