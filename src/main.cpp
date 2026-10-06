#include "ocr.h"
#include "window.h"
#ifdef TATSU_ENABLE_SELFTEST
#include "selftest.h"
#endif
#include <cstdio>
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("PDFTatsujin");
    app.setApplicationVersion("0.2.0-rc2");
    app.setOrganizationName("PDFTatsujin");
    try
    {
        // Use the shipped family for controls as well as PDF text. This keeps
        // Japanese labels available even when the platform font service differs.
        app.setFont(QFont(tatsu::signatureFont(), 10));
    }
    catch (const std::exception& e)
    {
        QMessageBox::critical(nullptr, "起動エラー", QString::fromUtf8(e.what()));
        return 1;
    }
    auto args = app.arguments();
    if (args.size() > 1 && args[1] == "--ocr-worker")
        return tatsu::ocrWorker(args.mid(1));
#ifdef TATSU_ENABLE_SELFTEST
    if (args.size() == 4 && args[1] == "--selftest")
        return tatsu::selftest(args[2], args[3]);
#endif
    if (args.size() == 4 && args[1] == "--measure")
    {
        try
        {
            QElapsedTimer timer;
            timer.start();
            tatsu::Document d;
            d.open(args[2]);
            auto opened = timer.elapsed();
            tatsu::Window window;
            window.doc = std::move(d);
            window.refresh(true);
            window.show();
            app.processEvents();
            auto visible = timer.elapsed();
            window.canvas->goToPage(0);
            while (!window.canvas->pageReady(0) && timer.elapsed() < 30000)
            {
                app.processEvents();
                QThread::msleep(1);
            }
            if (!window.canvas->pageReady(0))
                tatsu::fail("初回ページの描画が完了しませんでした。");
            auto image = window.grab();
            auto readable = timer.elapsed();
            image.save(args[3] + ".png");
            QEventLoop observation;
            QTimer::singleShot(250, &observation, &QEventLoop::quit);
            observation.exec();
            QFile out(args[3]);
            out.open(QIODevice::WriteOnly);
            out.write(QJsonDocument(QJsonObject{{"open_ms", opened},
                                                {"measurement_version", 2},
                                                {"first_readable_ms", readable},
                                                {"first_page_ms", readable - opened},
                                                {"window_visible_ms", visible},
                                                {"qt_platform", QGuiApplication::platformName()},
                                                {"observation_ms", 250},
                                                {"pages", window.doc.pages()},
                                                {"image_pixels", image.width() * image.height()}})
                          .toJson());
            return 0;
        }
        catch (...)
        {
            return 2;
        }
    }
    try
    {
        // Remove only marked, unlocked job folders owned by this application. Never follow
        // symlinks.
        QDir temp(QDir::tempPath());
        for (const auto& info : temp.entryInfoList(
                 {"pdf-tatsujin-job-*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        {
            QFile marker(info.absoluteFilePath() + "/.tatsujin-owner");
            if (!marker.open(QIODevice::ReadOnly) || marker.readAll() != "PDFTatsujin job v1")
                continue;
            marker.close();
            QLockFile lock(info.absoluteFilePath() + "/job.lock");
            lock.setStaleLockTime(0);
            if (lock.tryLock())
            {
                lock.unlock();
                QDir(info.absoluteFilePath()).removeRecursively();
            }
        }
        auto window = new tatsu::Window;
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        if (args.size() > 1)
            window->guard([&] { window->openFile(args[1]); });
        return app.exec();
    }
    catch (const std::exception& e)
    {
        QMessageBox::critical(nullptr, "起動エラー", QString::fromUtf8(e.what()));
        return 1;
    }
}
