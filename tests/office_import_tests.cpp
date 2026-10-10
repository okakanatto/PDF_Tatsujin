#include "office_import_tests.h"
#include "office_import.h"
#include "office_import_dialog.h"
#include "owned_process.h"
#include "window.h"
#include <QtTest/QTest>
#include <windows.h>

namespace tatsu
{
namespace
{
void check(bool ok, const QString& why)
{
    if (!ok)
        fail(why);
}
QJsonObject criteria(const QString& fixtures)
{
    const auto path = fixtures + "/office-import/criteria.json";
    check(fileHash(path).toHex() ==
              "18a8ddead69890390cf52cf9fca782563cd23c4d30dfdbed58e9bb6800ac3785",
          "Frozen Office criteria SHA");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read Office criteria");
    return QJsonDocument::fromJson(file.readAll()).object();
}
} // namespace
QJsonObject testOfficeImport(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    const auto hashes = fixed["files"].toObject();
    for (auto i = hashes.begin(); i != hashes.end(); ++i)
        check(fileHash(fixtures + "/office-import/" + i.key()).toHex() ==
                  i.value().toString().toLatin1(),
              "Frozen DOCX fixture SHA");
    for (const auto& name : {QString("simple.docx"), QString("pages-images.docx")})
    {
        const auto path = fixtures + "/office-import/" + name;
        const auto original = fileHash(path);
        auto candidate = importDocx(path, officeConverterPath(), {}, true);
        writeCandidate(candidate,
                       output + "/office-observed-" + QFileInfo(name).completeBaseName() + ".pdf");
        const auto pages = fixed["expected_pages"].toObject()[name].toArray();
        check(candidate.getCatalog()->getPageCount() == pages.size(), "Office page count");
        for (int page = 0; page < pages.size(); ++page)
        {
            const auto text = pageText(candidate, page);
            QFile diagnostic(output + "/office-observed-" + QFileInfo(name).completeBaseName() +
                             QString("-%1.txt").arg(page));
            check(diagnostic.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                  "Office text diagnostic");
            diagnostic.write(text.toUtf8());
            diagnostic.close();
            for (const auto& expected : pages[page].toArray())
                check(text.contains(expected.toString()),
                      "Office searchable text: " + expected.toString());
            const auto dimensions = pageSize(candidate.getCatalog()->getPage(page));
            const auto size = fixed["page_size_pt"].toArray();
            check(qAbs(dimensions.width() - size[0].toDouble()) <= .5 &&
                      qAbs(dimensions.height() - size[1].toDouble()) <= .5,
                  "Office A4 page geometry");
            check(!renderPage(candidate, page, 1).isNull(), "Office actual PDF render");
        }
        writeCandidate(candidate,
                       output + "/office-" + QFileInfo(name).completeBaseName() + ".pdf");
        check(fileHash(path) == original, "DOCX source unchanged");
        if (name == "simple.docx")
        {
            Document document;
            document.history = {candidate};
            document.saved = -1;
            const auto before = encodePdf(document.pdf());
            const auto signature =
                document.putSignature(0, "日本語の署名", {100, 400}, 16, Qt::black);
            document.moveSignature(0, signature, {20, 15});
            document.undo();
            document.undo();
            check(encodePdf(document.pdf()) == before, "Converted PDF signature Undo");
            document.redo();
            document.redo();
            document.save(output + "/office-signed.pdf");
            Document reopened;
            reopened.open(output + "/office-signed.pdf");
            const auto saved = signatures(reopened.pdf(), 0);
            check(saved.size() == 1 && saved[0].text == "日本語の署名",
                  "Converted PDF editable signature");
            reopened.moveSignature(0, saved[0], {5, 5});
            reopened.save(output + "/office-reedited.pdf");
        }
    }
    int refused = 0;
    for (const auto& entry : fixed["refused"].toArray())
    {
        try
        {
            validatedDocx(fixtures + "/office-import/" + entry.toString());
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    }
    check(refused == 4, "Office macro, embedding, external image, bad ZIP refused");
    const auto path = fixtures + "/office-import/simple.docx";
    const auto original = fileHash(path);
    const auto temporaryBefore =
        QDir(QDir::tempPath())
            .entryList({"PDFTatsujin-office-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    QElapsedTimer timer;
    timer.start();
    bool cancelled = false;
    try
    {
        importDocx(path, officeConverterPath(), [&] { return timer.elapsed() > 300; });
    }
    catch (const std::exception& error)
    {
        cancelled = QString::fromUtf8(error.what()).contains("取り消");
    }
    check(cancelled && fileHash(path) == original,
          "Actual converter cancelled with source retained");
    check(QDir(QDir::tempPath())
                  .entryList({"PDFTatsujin-office-*"}, QDir::Dirs | QDir::NoDotAndDotDot) ==
              temporaryBefore,
          "Office private input and profile removed after cancellation");
    return {{"spacing_option", "explicit suppression; source unchanged"},
            {"frozen_DOCX_pages_text_geometry_render", true},
            {"signature_Undo_save_reedit", true},
            {"refused", refused}};
}
QJsonObject testOfficeImportUi(const QString& fixtures, const QString& output)
{
    OfficeImportDialog dialog(fixtures + "/office-import/pages-images.docx");
    dialog.show();
    auto apply = dialog.findChild<QPushButton*>("officeImportApply");
    check(!apply->isEnabled(), "Office UI waits for conversion");
    check(QTest::qWaitFor([&] { return apply->isEnabled(); }, 120000),
          "Office UI conversion preview");
    auto spacing = dialog.findChild<QCheckBox*>("officeImportSuppressSpacing");
    check(!spacing->isChecked(), "Office UI retains input spacing by default");
    spacing->setFocus();
    QTest::keyClick(spacing, Qt::Key_Space);
    check(spacing->isChecked() && !apply->isEnabled(), "Office UI explicit spacing change");
    check(QTest::qWaitFor([&] { return apply->isEnabled(); }, 120000), "Office UI spacing preview");
    auto page = dialog.findChild<QSpinBox*>("officeImportPage");
    page->setFocus();
    QTest::keyClick(page, Qt::Key_Up);
    check(page->value() == 2, "Office UI keyboard page navigation");
    check(QTest::qWaitFor([&] { return apply->isEnabled(); }, 15000),
          "Office UI second page rendered");
    check(dialog.grab().save(output + "/office-import-ui.png"), "Actual Office UI screenshot");
    QTest::mouseClick(apply, Qt::LeftButton);
    check(dialog.result() == QDialog::Accepted, "Office UI explicit acceptance");
    auto document = dialog.takeDocument();
    check(document.getCatalog()->getPageCount() == 2, "Office UI candidate pages");
    writeCandidate(document, output + "/office-ui.pdf");
    OfficeImportDialog cancelled(fixtures + "/office-import/simple.docx");
    cancelled.show();
    QTest::qWait(250);
    QTest::mouseClick(cancelled.findChild<QPushButton*>("officeImportCancel"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return !cancelled.isVisible(); }, 15000),
          "Office UI cancellation closes");
    return {{"actual_preview_page_accept_cancel", true}, {"native_Word_IME", "未実行"}};
}
QJsonObject testOwnedProcess(const QString& output)
{
    const auto powershell =
        qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
    const auto marker = output + "/owned-child.pid";
    // This helper spawns a child before sleeping. No user process is addressed.
    const auto script = output + "/owned-process-helper.ps1";
    QFile file(script);
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Owned process helper creation");
    file.write(
        "param([string]$Marker)\n$p=Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') "
        "-ArgumentList '-NoProfile -Command Start-Sleep -Seconds 120' -WindowStyle Hidden "
        "-PassThru\n[IO.File]::WriteAllText($Marker,[string]$p.Id)\nStart-Sleep -Seconds 120\n");
    file.close();
    QProcess unrelated;
    unrelated.setProgram(powershell);
    unrelated.setArguments({"-NoProfile", "-Command", "Start-Sleep -Seconds 120"});
    unrelated.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args)
                                                { args->flags |= CREATE_NO_WINDOW; });
    unrelated.start();
    check(unrelated.waitForStarted(), "Own unrelated control helper starts");
    struct ControlCleanup
    {
        QProcess& process;
        ~ControlCleanup()
        {
            process.kill();
            process.waitForFinished();
        }
    } cleanup{unrelated};
    HANDLE child = nullptr;
    bool cancelled = false;
    QString cancellationError;
    try
    {
        runOwnedProcess(powershell, {"-NoProfile", "-File", script, marker}, output, 15000,
                        [&]
                        {
                            QFile pid(marker);
                            if (!pid.open(QIODevice::ReadOnly))
                                return false;
                            const auto id = pid.readAll().trimmed().toULong();
                            if (!id)
                                return false;
                            child = OpenProcess(SYNCHRONIZE, FALSE, id);
                            return child != nullptr;
                        });
    }
    catch (const std::exception& error)
    {
        cancellationError = QString::fromUtf8(error.what());
        cancelled = QString::fromUtf8(error.what()).contains("取り消");
    }
    const bool childExited = child && WaitForSingleObject(child, 5000) == WAIT_OBJECT_0;
    if (child)
        CloseHandle(child);
    check(cancelled && childExited,
          QString("Owned cancellation: cancelled=%1 childExited=%2 reason=%3")
              .arg(cancelled)
              .arg(childExited)
              .arg(cancellationError));
    check(unrelated.state() == QProcess::Running && !unrelated.waitForFinished(50),
          "Unrelated helper survives job cancellation");
    bool timeout = false;
    try
    {
        runOwnedProcess(powershell, {"-NoProfile", "-Command", "Start-Sleep -Seconds 120"}, output,
                        200);
    }
    catch (const std::exception& error)
    {
        timeout = QString::fromUtf8(error.what()).contains("制限時間");
    }
    check(timeout, "Owned process timeout terminates job");
    return {{"cancelled_descendant_exited", true}, {"unrelated_survives", true}, {"timeout", true}};
}
QJsonObject testOfficeImportWindow(const QString& fixtures, const QString& output)
{
    Window original;
    original.show();
    original.openFile(fixtures + "/D01.pdf");
    original.doc.putSignature(0, "原本の未保存署名", {80, 70}, 16, Qt::black);
    original.refresh();
    const auto before = encodePdf(original.doc.pdf());
    const auto cursor = original.doc.cursor;
    const auto saved = original.doc.saved;
    QTimer automation;
    QString error;
    QElapsedTimer elapsed;
    elapsed.start();
    bool spacingSelected = false;
    QObject::connect(&automation, &QTimer::timeout, &original,
                     [&]
                     {
                         auto dialog =
                             dynamic_cast<OfficeImportDialog*>(QApplication::activeModalWidget());
                         if (!dialog)
                             return;
                         if (elapsed.elapsed() > 120000)
                         {
                             error = "Office new-window conversion timeout";
                             dialog->reject();
                             return;
                         }
                         auto apply = dialog->findChild<QPushButton*>("officeImportApply");
                         if (!apply->isEnabled())
                             return;
                         if (!spacingSelected)
                         {
                             spacingSelected = true;
                             auto spacing =
                                 dialog->findChild<QCheckBox*>("officeImportSuppressSpacing");
                             spacing->setFocus();
                             QTest::keyClick(spacing, Qt::Key_Space);
                         }
                         else
                         {
                             automation.stop();
                             QTest::mouseClick(apply, Qt::LeftButton);
                         }
                     });
    automation.start(30);
    original.createFromDocx(fixtures + "/office-import/simple.docx");
    check(error.isEmpty(), error);
    Window* created = nullptr;
    for (const auto widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "officeCreatedDocument")
            created = dynamic_cast<Window*>(widget);
    check(created && created->doc.dirty() && created->doc.source.isEmpty(),
          "Office separate unsaved window");
    std::unique_ptr<Window> owner(created);
    check(encodePdf(original.doc.pdf()) == before && original.doc.cursor == cursor &&
              original.doc.saved == saved,
          "Office import retains original unsaved PDF and Undo");
    check(QTest::qWaitFor([&] { return created->canvas->pageReady(0); }, 15000),
          "Office new PDF visible");
    created->query->setText("検索できる日本語PDFです。");
    QTest::keyClick(created->query, Qt::Key_Return);
    const auto search = created->findChild<SearchPanel*>("searchPanel")->session();
    check(QTest::qWaitFor([&] { return search->complete() && search->rowCount() == 1; }, 15000),
          "Office actual UI Japanese search");
    check(search->errors().isEmpty(), "Office UI search error-free");
    for (const auto& phrase : {QString("検索できる日本語PDFです。"),
                               QString("English document conversion preserves searchable text.")})
    {
        const auto layout = textLayout(created->doc.pdf(), 0);
        bool copied = false;
        for (const auto& flow : PDFTextFlow::createTextFlows(layout, PDFTextFlow::AddLineBreaks, 0))
        {
            const auto at = flow.getText().indexOf(phrase);
            if (at < 0)
                continue;
            const auto boxes = flow.getBoundingBoxes();
            const auto first = boxes[size_t(at)];
            const auto last = boxes[size_t(at + phrase.size() - 1)];
            const auto a =
                created->canvas->pdfToViewport(0, {first.left() - .6, first.center().y()})
                    .toPoint();
            const auto b =
                created->canvas->pdfToViewport(0, {last.right() + .6, last.center().y()}).toPoint();
            auto viewport = created->canvas->viewport();
            check(viewport->rect().contains(a) && viewport->rect().contains(b),
                  "Office copy endpoints visible");
            QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, a);
            QTest::mouseMove(viewport, b, 20);
            QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, b);
            check(QTest::qWaitFor([&] { return created->canvas->selectionReady(); }, 15000),
                  "Office selection ready");
            QApplication::clipboard()->setText("Office clipboard sentinel");
            QTest::keyClick(viewport, Qt::Key_C, Qt::ControlModifier);
            check(QApplication::clipboard()->text() == phrase,
                  "Office actual Japanese/English copy");
            copied = true;
            break;
        }
        check(copied, "Office copy phrase found");
    }
    created->doc.save(output + "/office-window.pdf");
    check(created->grab().save(output + "/office-created-window.png"),
          "Office actual created window screenshot");
    return {{"separate_unsaved_window", true},
            {"original_unsaved_history_preserved", true},
            {"actual_Japanese_search_save", true},
            {"actual_Japanese_English_selection_copy", true}};
}
} // namespace tatsu
