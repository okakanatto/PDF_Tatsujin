#include "ocr.h"
#include "window.h"
#include <cstdio>

namespace
{
void require(bool value, const QString& message)
{
    if (!value)
        tatsu::fail(message);
}
void waitFor(const std::function<bool()>& condition, int timeout)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition())
    {
        require(timer.elapsed() < timeout, "Interaction benchmark timed out");
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
}
void mouse(QWidget* view, QEvent::Type type, QPoint point)
{
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, QPointF(point), QPointF(view->mapToGlobal(point)), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(view, &event);
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const auto args = app.arguments();
    try
    {
        app.setFont(QFont(tatsu::signatureFont(), 10));
        if (args.size() > 1 && args[1] == "--ocr-worker")
            return tatsu::ocrWorker(args.mid(1));
        require(args.size() == 3, "Expected fixture and output directories");
        const auto fixtures = QFileInfo(args[1]).absoluteFilePath();
        const auto output = QFileInfo(args[2]).absoluteFilePath();
        require(!QFileInfo::exists(output) && QDir().mkpath(output), "Output already exists");
        QJsonArray runs;
        for (int run = 0; run < 3; ++run)
        {
            tatsu::Window window;
            window.resize(1280, 850);
            window.show();
            window.openFile(fixtures + "/D01.pdf");
            const auto sourceHash = tatsu::fileHash(window.doc.source);
            window.doc.putSignature(0, "山田 太郎", {90, 400}, 18, Qt::black);
            window.refresh();
            waitFor([&] { return window.canvas->pageReady(0); }, 30000);
            const auto original = tatsu::signatures(window.doc.pdf(), 0).front();
            const auto start = window.canvas->pdfToViewport(0, original.rect.center()).toPoint();
            const auto cursor = window.doc.cursor;
            auto view = window.canvas->viewport();
            mouse(view, QEvent::MouseButtonPress, start);
            QJsonArray frames;
            for (int step = 1; step <= 30; ++step)
            {
                QElapsedTimer timer;
                timer.start();
                mouse(view, QEvent::MouseMove, start + QPoint(step, step / 2));
                QCoreApplication::processEvents();
                view->repaint();
                frames.append(timer.nsecsElapsed() / 1000000.0);
                require(window.doc.cursor == cursor, "Preview committed an intermediate drag");
            }
            QElapsedTimer commit;
            commit.start();
            mouse(view, QEvent::MouseButtonRelease, start + QPoint(30, 15));
            waitFor([&] { return window.canvas->pageReady(0); }, 30000);
            const auto commitMs = commit.nsecsElapsed() / 1000000.0;
            require(window.doc.cursor == cursor + 1, "Drag must commit once");
            window.undoAction->trigger();
            require(tatsu::signatures(window.doc.pdf(), 0).front().rect == original.rect,
                    "Drag Undo did not restore original");
            require(tatsu::fileHash(window.doc.source) == sourceHash, "Drag changed source");
            window.openFile(fixtures + "/D03.pdf");
            window.doc.putSignature(0, "OCR取消後も保持", {50, 40}, 16, Qt::black);
            window.refresh();
            waitFor([&] { return window.canvas->pageReady(0); }, 30000);
            const auto before = window.doc.pdf();
            const auto revision = window.doc.revision;
            window.startOcr();
            require(window.work && window.worker, "OCR did not start");
            const auto temporary = window.work->path();
            waitFor([&] { return !window.worker || window.progress->text().startsWith("OCR 1 /"); },
                    90000);
            require(window.worker, "Worker ended before cancellation");
            QElapsedTimer navigate;
            navigate.start();
            window.canvas->goToPage(1);
            waitFor([&] { return window.canvas->pageReady(1); }, 30000);
            const auto navigateMs = navigate.nsecsElapsed() / 1000000.0;
            require(window.cancel->isEnabled(), "Cancellation not available during OCR");
            QElapsedTimer cancellation;
            cancellation.start();
            window.cancel->click();
            waitFor([&] { return !window.worker && !window.doc.busy; }, 10000);
            const auto cancelMs = cancellation.nsecsElapsed() / 1000000.0;
            require(window.doc.pdf() == before && window.doc.revision == revision &&
                        window.doc.dirty() && !QFileInfo::exists(temporary),
                    "Cancellation did not preserve document or clean temporary job");
            window.grab().save(output + QString("/cancel-%1.png").arg(run));
            runs.append(QJsonObject{{"run", run + 1},
                                    {"drag_preview_event_and_repaint_ms", frames},
                                    {"drag_commit_until_page_ready_ms", commitMs},
                                    {"navigate_during_OCR_until_ready_ms", navigateMs},
                                    {"cancel_until_worker_cleanup_ms", cancelMs},
                                    {"drag_Undo_and_cancel_preservation", true}});
        }
        QFile report(output + "/interactions.json");
        require(report.open(QIODevice::WriteOnly), "Cannot write measurements");
        report.write(
            QJsonDocument(
                QJsonObject{{"status", "PASS"},
                            {"runs", runs},
                            {"qt_platform", QGuiApplication::platformName()},
                            {"scope", "shared product UI, synthetic Qt events and synchronous "
                                      "offscreen repaint; not physical pointer/display latency"}})
                .toJson());
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
