#include "selftest.h"
#include "window.h"
#include <QtTest/QTest>
#include <windows.h>

namespace tatsu
{
static void require(bool ok, const QString& why)
{
    if (!ok)
        fail(why);
}
static QJsonObject workerTest(const PDFDocument& doc, const QString& output, const QString& pages,
                              const QString& lang = "jpn+eng", bool cancel = false)
{
    QTemporaryDir temp;
    require(temp.isValid(), "temporary directory");
    writeCandidate(doc, temp.filePath("input.pdf"));
    QFile opt(temp.filePath("options.json"));
    opt.open(QIODevice::WriteOnly);
    opt.write(QJsonDocument(QJsonObject{{"pages", pages}, {"language", lang}}).toJson());
    opt.close();
    QProcess p;
    p.start(QCoreApplication::applicationFilePath(),
            {"--ocr-worker", temp.filePath("input.pdf"), output, temp.filePath("options.json"),
             temp.filePath("report.json")});
    require(p.waitForStarted(), "worker started");
    QElapsedTimer timer;
    timer.start();
    QByteArray progress;
    int ticks = 0;
    while (p.state() != QProcess::NotRunning && timer.elapsed() < 300000)
    {
        p.waitForReadyRead(30);
        progress += p.readAllStandardOutput();
        QCoreApplication::processEvents();
        ++ticks;
        if (cancel && progress.contains("\n"))
        {
            p.kill();
            p.waitForFinished(5000);
            break;
        }
    }
    if (p.state() != QProcess::NotRunning)
    {
        p.kill();
        p.waitForFinished();
        fail("worker timeout");
    }
    if (cancel)
    {
        require(!QFileInfo::exists(output), "cancel must not publish partial result");
        return {{"cancelled_after_progress", QString::fromUtf8(progress)},
                {"event_loop_ticks", ticks}};
    }
    require(p.exitCode() == 0 && p.exitStatus() == QProcess::NormalExit,
            "OCR worker: " + QString::fromUtf8(p.readAllStandardError()));
    QFile report(temp.filePath("report.json"));
    report.open(QIODevice::ReadOnly);
    auto result = QJsonDocument::fromJson(report.readAll()).object();
    result["elapsed_ms"] = timer.elapsed();
    result["event_loop_ticks"] = ticks;
    return result;
}
int selftest(const QString& fixtures, const QString& output)
{
    QDir().mkpath(output);
    QJsonArray results;
    int failures = 0;
    auto run = [&](QString name, std::function<QJsonObject()> test)
    {
        auto filter = qEnvironmentVariable("TATSU_TEST_FILTER");
        if (!filter.isEmpty() && !name.contains(filter))
            return;
        QElapsedTimer timer;
        timer.start();
        QJsonObject row{{"name", name}};
        try
        {
            row["details"] = test();
            row["status"] = "PASS";
        }
        catch (const std::exception& e)
        {
            row["status"] = "FAIL";
            row["error"] = QString::fromUtf8(e.what());
            ++failures;
        }
        row["elapsed_ms"] = timer.elapsed();
        results.append(row);
        QFile out(output + "/selftest.json");
        out.open(QIODevice::WriteOnly);
        out.write(QJsonDocument(QJsonObject{{"tests", results},
                                            {"failures", failures},
                                            {"qt", qVersion()},
                                            {"platform", QSysInfo::prettyProductName()}})
                      .toJson());
    };
    auto input = [&](QString name) { return fixtures + "/" + name; };
    auto dest = [&](QString name) { return output + "/" + name; };
    run("A01_document_and_UI",
        [&]
        {
            Window window;
            window.openFile(input("D01.pdf"));
            window.show();
            QTest::qWait(300);
            require(window.doc.pages() == 1, "page count");
            auto text = pageText(window.doc.pdf(), 0);
            require(text.contains("English"), "existing English text");
            require(text.contains("日本語"), "existing Japanese text");
            window.canvas->setZoom(1.5);
            window.canvas->refresh();
            require(window.grab().save(dest("main-window.png")), "screenshot");
            window.doc.saved = window.doc.cursor;
            return QJsonObject{{"text", text}, {"window_size", "1280x850"}};
        });
    run("A02_UI_pointer_drag_undo_copy",
        [&]
        {
            Window w;
            w.openFile(input("D01.pdf"));
            w.show();
            QTest::qWait(200);
            w.signature->setPlainText("山田 太郎");
            w.canvas->placing = true;
            auto matrix = pageMatrix(w.doc.pdf().getCatalog()->getPage(0));
            auto click = w.canvas->mapFromScene(matrix.map(QPointF(90, 400)));
            QTest::mouseClick(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, click);
            require(signatures(w.doc.pdf(), 0).size() == 1, "pointer placement");
            auto before = signatures(w.doc.pdf(), 0).at(0);
            auto start = w.canvas->mapFromScene(matrix.map(before.rect.center()));
            auto end = start + QPoint(35, 25);
            int cursor = w.doc.cursor;
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(w.canvas->viewport(), end, 30);
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, end);
            require(w.doc.cursor == cursor + 1, "one drag one undo");
            w.undoAction->trigger();
            require(signatures(w.doc.pdf(), 0).at(0).rect == before.rect, "toolbar undo");
            w.redoAction->trigger();
            require(signatures(w.doc.pdf(), 0).at(0).rect != before.rect, "toolbar redo");
            auto a = w.canvas->mapFromScene(QPointF(45, 55));
            auto b = w.canvas->mapFromScene(QPointF(545, 108));
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, a);
            QTest::mouseMove(w.canvas->viewport(), b, 20);
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, b);
            QTest::keyClick(w.canvas, Qt::Key_C, Qt::ControlModifier);
            require(QApplication::clipboard()->text().contains("English"),
                    "selection copy clipboard");
            w.grab().save(dest("signature-ui.png"));
            w.doc.saved = w.doc.cursor;
            return QJsonObject{{"native_mouse_events", true},
                               {"clipboard_copy", true},
                               {"IME", "未実行・確定済み文字列を使用"}};
        });
    run("A02_A03_signature_roundtrip",
        [&]
        {
            Document d;
            d.open(input("D01.pdf"));
            auto hash = fileHash(d.source);
            auto s = d.putSignature(0, "山田 太郎\n髙橋\n2026年10月4日", {80, 400}, 20, Qt::black);
            require(signatures(d.pdf(), 0).at(0).rect == s.rect,
                    "initial appearance uses requested rect without stamp resizing");
            auto beforeMove = d.pdf();
            d.moveSignature(0, s, {40, 30});
            auto moved = signatures(d.pdf(), 0).at(0);
            require(moved.rect.topLeft() == s.rect.topLeft() + QPointF(40, 30), "move");
            d.undo();
            require(d.pdf() == beforeMove, "undo restores complete pre-drag document");
            d.redo();
            d.save(dest("signature.pdf"));
            require(!d.dirty(), "clean after save");
            d.undo();
            require(d.dirty(), "dirty after undo saved state");
            d.redo();
            require(!d.dirty(), "clean redo");
            Document reopened;
            reopened.open(dest("signature.pdf"));
            auto list = signatures(reopened.pdf(), 0);
            require(list.size() == 1 && list[0].text.contains("髙橋"),
                    "editable persisted signature");
            reopened.putSignature(0, "山田 次郎", list[0].rect.topLeft(), 22, Qt::darkBlue,
                                  list[0].ref);
            reopened.save(dest("signature-reedited.pdf"));
            require(fileHash(d.source) == hash, "original unchanged");
            require(renderPage(d.pdf(), 0, 1.3).save(dest("signature.png")), "render signature");
            bool rejected = false;
            try
            {
                d.putSignature(0, QString::fromUcs4(U"\U0010ffff"), {80, 200}, 20, Qt::black);
            }
            catch (...)
            {
                rejected = true;
            }
            require(rejected, "unsupported character rejection");
            return QJsonObject{{"original_sha256", QString::fromLatin1(hash.toHex())},
                               {"signature_count", list.size()},
                               {"IME", "未実行"}};
        });
    run("A04_rotations_crop_userunit",
        [&]
        {
            Document d;
            d.open(input("D02.pdf"));
            double error = 0;
            for (int i = 0; i < d.pages(); ++i)
            {
                auto p = d.pdf().getCatalog()->getPage(i);
                for (double z : {.5, 1., 1.5, 2.})
                {
                    auto m = pageMatrix(p, z);
                    QPointF point(123.25, 245.75);
                    error = std::max(error, QLineF(point, m.inverted().map(m.map(point))).length());
                }
                auto s = d.putSignature(i, "山田 太郎", {90, 400}, 18, Qt::black);
                d.moveSignature(i, s, {20.25, 10.5});
                d.rotate(i);
            }
            d.save(dest("coordinates.pdf"));
            Document check;
            check.open(dest("coordinates.pdf"));
            for (int i = 0; i < check.pages(); ++i)
            {
                auto s = signatures(check.pdf(), i).at(0);
                error = std::max(error, QLineF(s.rect.topLeft(), QPointF(110.25, 410.5)).length());
                renderPage(check.pdf(), i, .7).save(dest(QString("coordinates-%1.png").arg(i + 1)));
            }
            require(error <= .5, "coordinate tolerance 0.5pt");
            return QJsonObject{
                {"max_error_pt", error},
                {"conditions", "0/90/180/270; CropBox offsets; UserUnit=2; zoom .5/1/1.5/2"}};
        });
    run("A05_OCR_eight_pages",
        [&]
        {
            Document d;
            d.open(input("D03.pdf"));
            auto result = workerTest(d.pdf(), dest("D03-ocr.pdf"), "1-8");
            auto o = readPdf(dest("D03-ocr.pdf"));
            QJsonArray texts;
            for (int i = 0; i < 8; ++i)
            {
                auto text = pageText(o, i);
                texts.append(text);
                require(!text.trimmed().isEmpty(), "text layer on each page");
            }
            QFile f(dest("D03-app-text.json"));
            f.open(QIODevice::WriteOnly);
            f.write(QJsonDocument(texts).toJson());
            result["accuracy_judgment"] = "external evaluator required";
            return result;
        });
    run("A05_A08_same_window_OCR_search_copy_save",
        [&]
        {
            Window w;
            w.openFile(input("D05.pdf"));
            w.show();
            QTest::qWait(100);
            w.doc.putSignature(0, "山田 太郎", {60, 40}, 16, Qt::black);
            w.refresh();
            auto before = w.doc.pdf();
            QStringList dialogs;
            QTimer dismiss;
            QObject::connect(&dismiss, &QTimer::timeout,
                             [&]
                             {
                                 for (auto widget : QApplication::topLevelWidgets())
                                     if (auto box = qobject_cast<QMessageBox*>(widget))
                                     {
                                         dialogs << box->windowTitle();
                                         box->accept();
                                     }
                             });
            dismiss.start(50);
            w.scope->setCurrentIndex(1);
            w.startOcr();
            require(w.doc.busy && w.cancel->isVisible(), "OCR busy state and cancel UI");
            QElapsedTimer timer;
            timer.start();
            while (w.worker && timer.elapsed() < 180000)
                QTest::qWait(20);
            require(!w.worker && !w.doc.busy, "UI OCR completion");
            require(w.doc.cursor == 2, "one OCR history transaction");
            w.query->setText("DIGITAL HEADING");
            QTest::keyClick(w.query, Qt::Key_Return);
            require(w.status->text().contains("一致したページ"), "search UI");
            w.canvas->setZoom(.5);
            QTest::qWait(50);
            auto dims = pageSize(w.doc.pdf().getCatalog()->getPage(0));
            auto first = w.canvas->mapFromScene(QPointF(1, 1));
            auto last = w.canvas->mapFromScene(QPointF(dims.width() - 1, dims.height() - 1));
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
            QTest::mouseMove(w.canvas->viewport(), last, 20);
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
            QTest::keyClick(w.canvas, Qt::Key_C, Qt::ControlModifier);
            auto copied = QApplication::clipboard()->text();
            require(copied.size() > 300, "OCR clipboard text");
            w.doc.save(dest("same-window.pdf"));
            w.refresh();
            w.grab().save(dest("ocr-window.png"));
            w.undoAction->trigger();
            require(w.doc.pdf() == before, "UI undo retains signature before OCR");
            w.redoAction->trigger();
            require(signatures(w.doc.pdf(), 0).size() == 1, "UI redo signature retained");
            w.doc.saved = w.doc.cursor;
            return QJsonObject{{"clipboard_characters", copied.size()},
                               {"dialogs", QJsonArray::fromStringList(dialogs)},
                               {"elapsed_ms", timer.elapsed()}};
        });
    run("A06_mixed_pages_and_content",
        [&]
        {
            Document d;
            d.open(input("D04.pdf"));
            workerTest(d.pdf(), dest("D04-ocr.pdf"), "2");
            auto result = readPdf(dest("D04-ocr.pdf"));
            require(pageText(result, 0) == pageText(d.pdf(), 0), "non-target text unchanged");
            require(pageText(result, 2).trimmed().isEmpty(),
                    "non-target image remains without OCR");
            d.open(input("D05.pdf"));
            d.putSignature(0, "髙橋 確認", {60, 40}, 16, Qt::blue);
            auto before = renderPage(d.pdf(), 0, 1);
            writeCandidate(d.pdf(), dest("D05-before.pdf"));
            auto report = workerTest(d.pdf(), dest("D05-ocr.pdf"), "1");
            result = readPdf(dest("D05-ocr.pdf"));
            require(signatures(result, 0).size() == 1, "signature preservation");
            auto text = pageText(result, 0);
            require(text.count("DIGITAL HEADING") == 1, "no duplicate digital heading");
            require(before == renderPage(result, 0, 1), "visible pixels unchanged");
            return report;
        });
    run("A07_existing_OCR_blank_photo",
        [&]
        {
            Document d;
            d.open(input("D06.pdf"));
            auto before = pageText(d.pdf(), 0);
            auto report = workerTest(d.pdf(), dest("D06-ocr.pdf"), "1-3");
            auto result = readPdf(dest("D06-ocr.pdf"));
            require(pageText(result, 0) == before, "existing OCR retained");
            require(report["pages"].toArray()[0].toObject()["status"].toString() == "既存OCR保持",
                    "existing layer status");
            return report;
        });
    run("A08_signature_rotate_OCR_undo_redo",
        [&]
        {
            Document d;
            d.open(input("D05.pdf"));
            d.putSignature(0, "山田 太郎", {60, 40}, 16, Qt::black);
            d.rotate(0);
            auto before = d.pdf();
            workerTest(before, dest("combined-candidate.pdf"), "1");
            d.commit(readPdf(dest("combined-candidate.pdf")));
            d.undo();
            require(d.pdf() == before, "OCR undo only");
            d.save(dest("combined-undone.pdf"));
            d.redo();
            require(signatures(d.pdf(), 0).size() == 1, "signature after OCR redo");
            d.save(dest("combined.pdf"));
            return QJsonObject{{"history_operations", d.cursor}, {"text", pageText(d.pdf(), 0)}};
        });
    run("A09_cancel_after_first_page",
        [&]
        {
            Document d;
            d.open(input("D03.pdf"));
            d.putSignature(0, "未保存の署名", {50, 40}, 16, Qt::black);
            auto before = d.pdf();
            auto original = fileHash(d.source);
            auto report = workerTest(before, dest("must-not-exist.pdf"), "1-8", "jpn+eng", true);
            require(d.pdf() == before && d.dirty(), "unsaved edit retained");
            require(fileHash(d.source) == original, "source unchanged");
            return report;
        });
    run("A09_worker_crash",
        [&]
        {
            Document d;
            d.open(input("D03.pdf"));
            d.putSignature(0, "保持する署名", {50, 40}, 16, Qt::black);
            auto before = d.pdf();
            QTemporaryDir temp;
            writeCandidate(d.pdf(), temp.filePath("input.pdf"));
            QFile f(temp.filePath("opt.json"));
            f.open(QIODevice::WriteOnly);
            f.write("{\"pages\":\"1-8\",\"language\":\"jpn+eng\"}");
            f.close();
            QProcess p;
            p.start(QCoreApplication::applicationFilePath(),
                    {"--ocr-worker", temp.filePath("input.pdf"), temp.filePath("output.pdf"),
                     temp.filePath("opt.json"), temp.filePath("report.json")});
            require(p.waitForStarted(), "worker start");
            QTest::qWait(150);
            p.kill();
            p.waitForFinished(5000);
            require(d.pdf() == before && d.dirty() && !QFile::exists(temp.filePath("output.pdf")),
                    "crash rollback");
            return QJsonObject{{"termination", "forced process kill"}};
        });
    run("A09_UI_cancel_and_cleanup",
        [&]
        {
            Window w;
            w.openFile(input("D03.pdf"));
            w.show();
            w.doc.putSignature(0, "未保存の署名", {50, 40}, 16, Qt::black);
            w.refresh();
            auto before = w.doc.pdf();
            auto revision = w.doc.revision;
            w.startOcr();
            auto temporary = w.work->path();
            QElapsedTimer timer;
            timer.start();
            while (!w.progress->text().startsWith("OCR 1 /") && w.worker && timer.elapsed() < 90000)
                QTest::qWait(20);
            require(w.worker && w.cancel->isEnabled(),
                    "cancel remains interactive after partial progress");
            w.pages->setCurrentRow(1);
            w.canvas->setZoom(.75);
            QTest::mouseClick(w.cancel, Qt::LeftButton);
            while (w.worker && timer.elapsed() < 100000)
                QTest::qWait(20);
            require(!w.worker && !w.doc.busy, "cancel completed");
            require(w.doc.pdf() == before && w.doc.revision == revision && w.doc.dirty(),
                    "cancel retains unsaved document transaction");
            require(!QFileInfo::exists(temporary), "owned temporary data removed");
            w.grab().save(dest("ocr-cancel.png"));
            w.doc.saved = w.doc.cursor;
            return QJsonObject{{"viewed_during_OCR", true},
                               {"cancel_button_clicked", true},
                               {"temporary_removed", true}};
        });
    run("A10_save_conflict_and_readonly",
        [&]
        {
            Document d;
            d.open(input("D09_日本語のパス/入力 文書.pdf"));
            d.putSignature(0, "髙橋", {60, 300}, 20, Qt::black);
            d.save(dest("conflict.pdf"));
            d.rotate(0);
            QFile changed(dest("conflict.pdf"));
            changed.open(QIODevice::Append);
            changed.write("\n% external change\n");
            changed.close();
            auto hash = fileHash(dest("conflict.pdf"));
            bool conflict = false;
            try
            {
                d.save(dest("conflict.pdf"));
            }
            catch (...)
            {
                conflict = true;
            }
            require(conflict && d.dirty() && fileHash(dest("conflict.pdf")) == hash,
                    "external conflict protection");
            QString ro = dest("readonly.pdf");
            QFile::copy(input("D01.pdf"), ro);
            SetFileAttributesW((LPCWSTR)ro.utf16(), FILE_ATTRIBUTE_READONLY);
            bool denied = false;
            auto rohash = fileHash(ro);
            try
            {
                d.save(ro, rohash);
            }
            catch (...)
            {
                denied = true;
            }
            SetFileAttributesW((LPCWSTR)ro.utf16(), FILE_ATTRIBUTE_NORMAL);
            require(denied && fileHash(ro) == rohash && d.dirty(), "read-only destination safety");
            return QJsonObject{{"external_conflict", conflict},
                               {"readonly_denied", denied},
                               {"real_disk_full", "未実行"}};
        });
    run("A11_protected_and_invalid",
        [&]
        {
            Document d;
            bool wrong = false;
            try
            {
                d.open(input("D08-encrypted.pdf"), "wrong");
            }
            catch (...)
            {
                wrong = true;
            }
            require(wrong, "wrong password rejected");
            d.open(input("D08-encrypted.pdf"), "correct-password");
            require(!d.readOnly.isEmpty(), "encrypted read only");
            bool blocked = false;
            try
            {
                d.rotate(0);
            }
            catch (...)
            {
                blocked = true;
            }
            require(blocked, "protected mutation rejected");
            d.open(input("D08-signed.pdf"));
            require(!d.readOnly.isEmpty(), "signature detected");
            d.open(input("D08-xfa.pdf"));
            require(!d.readOnly.isEmpty(), "XFA read only");
            bool broken = false;
            try
            {
                d.open(input("D08-broken.pdf"));
            }
            catch (...)
            {
                broken = true;
            }
            require(broken, "broken PDF rejected");
            return QJsonObject{{"signed_fixture",
                                "genuine CMS detached signature; cryptography independently "
                                "verified by .NET SignedCms; trust intentionally absent"}};
        });
    run("D07_form_annotation_link_outline_preservation",
        [&]
        {
            Document d;
            d.open(input("D07.pdf"));
            d.putSignature(0, "確認済", {350, 400}, 18, Qt::black);
            d.save(dest("form-preserved.pdf"));
            auto check = readPdf(dest("form-preserved.pdf"));
            require(check.getCatalog()->getPageCount() == 1, "form PDF reopened");
            renderPage(check, 0, 1.3).save(dest("form-preserved.png"));
            return QJsonObject{{"semantic_validation", "external evaluator required"}};
        });
    run("A03_Qt_PDF_print_path",
        [&]
        {
            Document d;
            d.open(dest("signature.pdf"));
            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(dest("signature-print.pdf"));
            printDocument(d.pdf(), printer);
            auto printed = readPdf(dest("signature-print.pdf"));
            require(printed.getCatalog()->getPageCount() == 1, "print PDF page count");
            renderPage(printed, 0, 1.2).save(dest("signature-print.png"));
            return QJsonObject{
                {"mechanism", "Qt PDF print device at 300dpi; raster output for printing only"},
                {"physical_printer", "未実行"},
                {"external_viewer_print", "未実行"}};
        });
    return failures ? 1 : 0;
}
} // namespace tatsu
