#include "batch_tests.h"
#include "annotation_operations.h"
#include "batch_dialog.h"
#include "bookmark_edit.h"
#include "form_fields.h"
#include "link_edit.h"
#include "page_decoration.h"
#include "page_operations.h"
#include "pdf_objects.h"
#include "pdf_optimization.h"
#include "window.h"
#include <QtTest/QTest>
#include <windows.h>

// Tool Help requires the Windows types above.
#include <tlhelp32.h>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
template <typename F> QString rejects(F operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
    fail("Invalid batch operation succeeded");
}
QString folder(const QString& output, const QString& name)
{
    const auto path = output + "/" + name;
    check(!QFileInfo::exists(path) && QDir().mkpath(path), "Create fresh batch output folder");
    return path;
}
PDFDocument seeded(const PDFDocument& source)
{
    auto storage = source.getStorage();
    PDFDictionary stream;
    detail::set(stream, "Filter", PDFObject::createName("FlateDecode"));
    const auto data = qCompress(QByteArray(200000, 'a'), 1).mid(4);
    const auto first = storage.addObject(detail::streamObject(stream, data));
    const auto second = storage.addObject(detail::streamObject(stream, data));
    storage.addObject(detail::streamObject({}, QByteArray(300000, 'x')));
    const auto root = source.getTrailerDictionary()->get("Root").getReference();
    auto catalog = *storage.getObjectByReference(root).getDictionary();
    detail::set(
        catalog, "BatchSyntheticData",
        detail::arrObject({PDFObject::createReference(first), PDFObject::createReference(second)}));
    storage.setObject(root, detail::dictObject(catalog));
    return PDFDocument(std::move(storage), source.getInfo()->version, source.getSourceDataHash());
}
PDFDocument rich(const QString& fixtures)
{
    Document document;
    document.history = {
        mergeDocuments({readPdf(fixtures + "/D02.pdf"), readPdf(fixtures + "/D07.pdf")})};
    document.saved = -1;
    document.putSignature(0, "一括処理の日本語署名", {100, 230}, 12, Qt::blue);
    putAnnotation(document, 0, OverlayKind::Comment, {80, 350, 24, 24}, "保持するコメント", Qt::red,
                  1);
    QImage image(80, 50, QImage::Format_RGB32);
    image.fill(QColor("#65a3ed"));
    document.putImage(0, OverlayKind::Image, image, {150, 350}, 80);
    auto links = editableLinks(document.pdf());
    links << LinkEntry{0, -1, {20, 20, 120, 35}, "保持するリンク", LinkTarget::Page, 2, {}};
    document.commit(replaceLinks(document.pdf(), links));
    document.commit(replaceBookmarks(document.pdf(), {{{}, "一括処理しおり", -1, 3, true, true}}));
    DecorationOptions watermark;
    watermark.kind = DecorationKind::Watermark;
    watermark.watermark = "資料";
    watermark.fontFamily = signatureFont();
    document.commit(putDecoration(document.pdf(), {0, 1}, watermark));
    for (const auto& field : formFields(document.pdf()))
        if (field.qualifiedName == "name")
            putFormValue(document, field.widget, {"髙橋 香織"});
    return seeded(document.pdf());
}
QJsonArray resultJson(const QVector<BatchItemResult>& rows)
{
    QJsonArray values;
    for (const auto& row : rows)
        values << QJsonObject{{"source", QFileInfo(row.source).fileName()},
                              {"output", QFileInfo(row.destination).fileName()},
                              {"status", batchStatusText(row.status)},
                              {"message", row.message},
                              {"outputSHA256", QString::fromLatin1(row.outputHash.toHex())}};
    return values;
}
void saveResults(const QVector<BatchItemResult>& rows, const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Create batch result evidence");
    file.write(QJsonDocument(resultJson(rows)).toJson());
}
bool killOwnedWorker()
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    check(snapshot != INVALID_HANDLE_VALUE, "Enumerate own child for crash test");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool killed = false;
    if (Process32FirstW(snapshot, &entry))
        do
        {
            if (entry.th32ParentProcessID != GetCurrentProcessId())
                continue;
            const auto process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                             FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768];
            DWORD length = 32768;
            if (QueryFullProcessImageNameW(process, 0, path, &length) &&
                sameFilePath(QString::fromWCharArray(path, int(length)),
                             QCoreApplication::applicationFilePath()))
                killed = TerminateProcess(process, 91) != 0;
            CloseHandle(process);
            if (killed)
                break;
        } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return killed;
}
} // namespace
QJsonObject testBatchLifecycle(const QString& fixtures, const QString& output)
{
    const auto before = rich(fixtures);
    const auto input = output + "/batch-rich.pdf", second = output + "/batch-別の資料.pdf";
    writeCandidate(before, input);
    writeCandidate(before, second);
    const auto noChange = output + "/batch-already-optimized.pdf";
    writeCandidate(optimizePdf(before).document, noChange);
    const auto target = folder(output, "batch-optimization");
    const auto plan = prepareBatch({input, second, noChange}, target, BatchOperation::Optimize);
    const auto firstHash = fileHash(input), secondHash = fileHash(second),
               unchangedHash = fileHash(noChange);
    int progress = 0;
    const auto rows = processBatch(plan, {}, [&](int, const BatchItemResult&) { ++progress; });
    check(rows.size() == 3 && rows[0].status == BatchStatus::Saved &&
              rows[1].status == BatchStatus::Saved && rows[2].status == BatchStatus::Unneeded,
          "Two saved optimized PDFs and exact no-change status");
    check(progress > 3 && fileHash(input) == firstHash && fileHash(second) == secondHash &&
              fileHash(noChange) == unchangedHash,
          "Real progress and input hashes retained");
    check(!QFileInfo::exists(rows[2].destination), "No unnecessary copied output");
    for (int n : {0, 1})
    {
        auto after = readPdf(rows[n].destination);
        auto source = before;
        check(QFileInfo(rows[n].destination).size() < QFileInfo(input).size() &&
                  fileHash(rows[n].destination) == rows[n].outputHash,
              "Smaller verified output and saved hash");
        for (int page = 0; page < 5; ++page)
            check(renderPage(source, page, .6) == renderPage(after, page, .6) &&
                      pageText(source, page) == pageText(after, page),
                  "All physical pages and visible text identical");
        check(editableLinks(after).size() == editableLinks(source).size() &&
                  editableBookmarks(after).size() == 1 && decorationGroups(after).size() == 1,
              "Links, bookmarks and editable decoration preserved");
        bool form = false;
        for (const auto& field : formFields(after))
            if (field.qualifiedName == "name")
                form = field.values == QStringList{"髙橋 香織"};
        check(form, "Japanese form value retained");
    }
    Document reopened;
    reopened.open(rows[0].destination);
    const auto mark = signatures(reopened.pdf(), 0)[0];
    reopened.moveSignature(0, mark, {10, 10});
    reopened.undo();
    reopened.redo();
    reopened.save(output + "/batch-reedited.pdf");
    check(!signatures(readPdf(reopened.target), 0).isEmpty(),
          "Saved output remains reeditable with Undo/Redo");
    saveResults(rows, output + "/batch-optimization-results.json");
    return {{"two_smaller_outputs", true},
            {"no_change_no_output", true},
            {"pixels_text_forms_related_data", true},
            {"inputs_unchanged", true},
            {"saved_reedit_Undo_Redo", true}};
}
QJsonObject testBatchFailures(const QString& fixtures, const QString& output)
{
    const auto input = output + "/batch-failure-input.pdf";
    writeCandidate(seeded(readPdf(fixtures + "/D01.pdf")), input);
    const auto initial = fileHash(input);
    const auto target = folder(output, "batch-failures");
    QJsonArray rejected;
    auto invalid = [&](QString name, const std::function<void()>& run)
    {
        rejected << QJsonObject{{"case", name}, {"error", rejects(run)}};
        check(fileHash(input) == initial, "Invalid settings preserve input");
    };
    invalid("empty input", [&] { prepareBatch({}, target, BatchOperation::Optimize); });
    invalid("duplicate input",
            [&] { prepareBatch({input, input}, target, BatchOperation::Optimize); });
    invalid("101 inputs",
            [&] { prepareBatch(QStringList(101, input), target, BatchOperation::Optimize); });
    invalid("invalid language",
            [&] { prepareBatch({input}, target, BatchOperation::Ocr, "unknown"); });
    invalid("invalid operation", [&] { prepareBatch({input}, target, BatchOperation(99)); });
    invalid("missing folder",
            [&] { prepareBatch({input}, target + "/missing", BatchOperation::Optimize); });
    const auto nested = folder(output, "batch-name-collision");
    const auto duplicate = nested + "/batch-failure-input.pdf";
    writeCandidate(readPdf(input), duplicate);
    invalid("same output name",
            [&] { prepareBatch({input, duplicate}, target, BatchOperation::Optimize); });
    {
        QTemporaryFile oversized(output + "/batch-too-large-XXXXXX.pdf");
        check(oversized.open() && oversized.resize(256ll * 1024 * 1024 + 1),
              "Actual file size above 256MiB");
        invalid("over 256MiB input",
                [&] { prepareBatch({oversized.fileName()}, target, BatchOperation::Optimize); });
    }
    const auto boundaryFolder = folder(output, "batch-100-inputs");
    QStringList hundred;
    for (int i = 0; i < 100; ++i)
    {
        const auto name = boundaryFolder + QString("/input-%1.pdf").arg(i);
        check(QFile::copy(fixtures + "/D01.pdf", name), "Create distinct boundary input");
        hundred << name;
    }
    check(prepareBatch(hundred, target, BatchOperation::Optimize).inputs.size() == 100,
          "100 distinct inputs accepted at preparation boundary");
    auto repeated = [&](int count)
    {
        auto source = seeded(readPdf(fixtures + "/D01.pdf"));
        auto storage = source.getStorage();
        auto page =
            *storage.getObjectByReference(source.getCatalog()->getPage(0)->getPageReference())
                 .getDictionary();
        const auto parent = page.get("Parent").getReference();
        std::vector<PDFObject> children;
        for (int i = 0; i < count; ++i)
            children.push_back(
                PDFObject::createReference(storage.addObject(detail::dictObject(page))));
        auto dictionary = *storage.getObjectByReference(parent).getDictionary();
        detail::set(dictionary, "Count", PDFObject::createInteger(count));
        detail::set(dictionary, "Kids", detail::arrObject(children));
        storage.setObject(parent, detail::dictObject(dictionary));
        return PDFDocument(std::move(storage), source.getInfo()->version,
                           source.getSourceDataHash());
    };
    const auto boundaryPages = output + "/batch-500-pages.pdf",
               excessivePages = output + "/batch-501-pages.pdf";
    writeCandidate(repeated(500), boundaryPages);
    writeCandidate(repeated(501), excessivePages);
    const auto pageFolder = folder(output, "batch-page-boundary");
    const auto pageRows = processBatch(
        prepareBatch({excessivePages, boundaryPages}, pageFolder, BatchOperation::Optimize));
    check(pageRows[0].status == BatchStatus::Failed &&
              !QFileInfo::exists(pageRows[0].destination) &&
              pageRows[1].status == BatchStatus::Saved &&
              readPdf(pageRows[1].destination).getCatalog()->getPageCount() == 500,
          "501-page failure continues to successful 500-page boundary");
    const auto plan = prepareBatch({input}, target, BatchOperation::Optimize);
    QFile collision(plan.inputs[0].destination);
    check(collision.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          "Create concurrent destination");
    collision.write("KEEP-BATCH-OUTPUT");
    collision.close();
    const auto collisionHash = fileHash(collision.fileName());
    invalid("existing output", [&] { prepareBatch({input}, target, BatchOperation::Optimize); });
    auto failed = processBatch(plan);
    check(failed[0].status == BatchStatus::Failed &&
              fileHash(collision.fileName()) == collisionHash,
          "Prepared plan cannot overwrite a later existing output");
    const auto raceFolder = folder(output, "batch-publication-race");
    const auto race = prepareBatch({input}, raceFolder, BatchOperation::Optimize);
    bool created = false;
    auto raced = processBatch(race, {},
                              [&](int, const BatchItemResult& row)
                              {
                                  if (!created && row.message.contains("最終確認"))
                                  {
                                      QFile file(race.inputs[0].destination);
                                      check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                                            "Create actual final publication conflict");
                                      file.write("KEEP-RACE");
                                      created = true;
                                  }
                              });
    check(created && raced[0].status == BatchStatus::Failed &&
              fileHash(race.inputs[0].destination) ==
                  QCryptographicHash::hash("KEEP-RACE", QCryptographicHash::Sha256),
          "Atomic publication conflict keeps competing file");
    const auto cancelledFolder = folder(output, "batch-written-cancel");
    const auto cancelledPlan = prepareBatch({input}, cancelledFolder, BatchOperation::Optimize);
    bool cancel = false;
    auto cancelledRows = processBatch(
        cancelledPlan, [&] { return cancel; },
        [&](int, const BatchItemResult& row)
        {
            if (row.message.contains("最終確認"))
                cancel = true;
        });
    check(cancel && cancelledRows[0].status == BatchStatus::Cancelled &&
              !QFileInfo::exists(cancelledPlan.inputs[0].destination),
          "Written candidate cancelled before publication");
    const auto updatedFolder = folder(output, "batch-input-updated");
    const auto updateInput = output + "/batch-updated-input.pdf";
    writeCandidate(readPdf(input), updateInput);
    const auto updatedPlan = prepareBatch({updateInput}, updatedFolder, BatchOperation::Optimize);
    bool updated = false;
    auto updatedRows = processBatch(updatedPlan, {},
                                    [&](int, const BatchItemResult& row)
                                    {
                                        if (!updated && row.message.contains("最終確認"))
                                        {
                                            QFile file(updateInput);
                                            check(file.open(QIODevice::Append),
                                                  "Update own synthetic input before publication");
                                            file.write("\n% updated\n");
                                            updated = true;
                                        }
                                    });
    check(updated && updatedRows[0].status == BatchStatus::Failed &&
              !QFileInfo::exists(updatedPlan.inputs[0].destination),
          "Updated input cannot publish stale output");
    const auto sequenceFolder = folder(output, "batch-partial-failure");
    const auto sequence =
        prepareBatch({fixtures + "/D08-broken.pdf", fixtures + "/D08-encrypted.pdf",
                      fixtures + "/D08-signed.pdf", fixtures + "/D08-xfa.pdf", input},
                     sequenceFolder, BatchOperation::Optimize);
    auto rows = processBatch(sequence);
    check(rows.size() == 5 && rows[4].status == BatchStatus::Saved,
          "Individual failure continues to valid input");
    for (int i = 0; i < 4; ++i)
        check(rows[i].status == BatchStatus::Failed && !QFileInfo::exists(rows[i].destination),
              "Broken and protected inputs not silently copied/decrypted");
    auto early = processBatch(sequence, [] { return true; });
    for (const auto& row : early)
        check(row.status == BatchStatus::NotProcessed, "Initial cancellation starts no input");
    const auto denied = qEnvironmentVariable("TATSU_DENIED_SAVE_DIR");
    QString denial = "未実行";
    if (!denied.isEmpty())
    {
        const auto deniedRows =
            processBatch(prepareBatch({input}, denied, BatchOperation::Optimize));
        check(deniedRows[0].status == BatchStatus::Failed &&
                  !QFileInfo::exists(deniedRows[0].destination),
              "Actual NTFS rejection publishes no output");
        denial = "PASS";
    }
    saveResults(rows, output + "/batch-partial-results.json");
    return {{"rejected", rejected},
            {"atomic_conflict_input_update_written_cancel", true},
            {"partial_failure_continues", true},
            {"NTFS_denial", denial},
            {"100_input_preparation_boundary", true},
            {"500_page_processing_boundary", true},
            {"volume_full", "未実行"}};
}
QJsonObject testBatchUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.open(fixtures + "/D02.pdf");
    window.doc.putSignature(0, "一括画面の元文書", {100, 230}, 12, Qt::blue);
    window.refresh(true);
    window.show();
    const auto snapshot = window.doc.pdf();
    const auto cursor = window.doc.cursor;
    const auto revision = window.doc.revision;
    const auto sourceHash = fileHash(window.doc.source);
    const auto first = output + "/batch-ui-first.pdf", second = output + "/batch-ui-second.pdf";
    writeCandidate(seeded(readPdf(fixtures + "/D01.pdf")), first);
    writeCandidate(seeded(readPdf(fixtures + "/D01.pdf")), second);
    const auto target = folder(output, "batch-ui-output");
    QString error;
    int state = 0, ticks = 0;
    QElapsedTimer deadline;
    deadline.start();
    QTimer drive;
    drive.setInterval(3);
    drive.setTimerType(Qt::PreciseTimer);
    QObject::connect(
        &drive, &QTimer::timeout,
        [&]
        {
            ++ticks;
            auto dialog = dynamic_cast<BatchDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            try
            {
                check(deadline.elapsed() < 45000, "Batch UI deadline");
                auto start = dialog->findChild<QPushButton*>("batchStart");
                auto cancel = dialog->findChild<QPushButton*>("batchCancel");
                auto table = dialog->findChild<QTableWidget*>("batchFiles");
                auto directory = dialog->findChild<QLineEdit*>("batchDirectory");
                if (state == 0)
                {
                    dialog->resize(800, 480);
                    dialog->setInputs({second, first});
                    table->selectRow(1);
                    QTest::mouseClick(dialog->findChild<QPushButton*>("batchUp"), Qt::LeftButton);
                    check(table->item(0, 0)->toolTip() == first, "Actual input move up");
                    QTest::mouseClick(dialog->findChild<QPushButton*>("batchDown"), Qt::LeftButton);
                    check(table->item(1, 0)->toolTip() == first, "Actual input move down");
                    QTest::mouseClick(dialog->findChild<QPushButton*>("batchRemove"),
                                      Qt::LeftButton);
                    check(table->rowCount() == 1, "Actual input remove");
                    dialog->setInputs({first, second});
                    dialog->findChild<QComboBox*>("batchOperation")->setCurrentIndex(1);
                    check(!dialog->findChild<QComboBox*>("batchLanguage")->isEnabled(),
                          "Language irrelevant to optimization is disabled");
                    directory->setText(target + "/missing");
                    QTest::mouseClick(start, Qt::LeftButton);
                    state = 1;
                }
                else if (state == 1 && start->isEnabled())
                {
                    check(dialog->results().isEmpty() &&
                              !dialog->findChild<QLabel*>("batchMessage")->text().isEmpty(),
                          "Failed preflight has no partial results");
                    directory->setText(target);
                    QTest::mouseClick(start, Qt::LeftButton);
                    state = 2;
                }
                else if (state == 2 && start->isEnabled() && !dialog->results().isEmpty())
                {
                    check(dialog->results().size() == 2 &&
                              dialog->results()[0].status == BatchStatus::Saved &&
                              dialog->results()[1].status == BatchStatus::Saved,
                          "Actual owned UI worker saves two PDFs");
                    check(start->isVisible() && cancel->isVisible() && directory->isVisible(),
                          "Compact dialog keeps principal controls visible");
                    check(dialog->grab().save(output + "/batch-ui.png"),
                          "Capture actual compact UI");
                    QTest::mouseClick(start, Qt::LeftButton);
                    state = 3;
                }
                else if (state == 3 && start->isEnabled())
                {
                    check(dialog->results().size() == 2 &&
                              dialog->results()[0].status == BatchStatus::Saved &&
                              dialog->results()[1].status == BatchStatus::Saved,
                          "Rejected rerun preserves previous successful output statuses");
                    state = 4;
                    QTest::mouseClick(cancel, Qt::LeftButton);
                }
            }
            catch (const std::exception& failure)
            {
                error = QString::fromUtf8(failure.what());
                drive.stop();
                QMetaObject::invokeMethod(dialog, "reject", Qt::QueuedConnection);
            }
        });
    drive.start();
    window.processMultipleDocuments();
    drive.stop();
    check(error.isEmpty() && state == 4,
          error.isEmpty() ? "Actual batch UI stages complete" : error);
    const auto cancelInput = output + "/batch-ui-cancel-scan.pdf";
    writeCandidate(selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6}), cancelInput);
    const auto cancelOutput = folder(output, "batch-ui-cancel-output");
    int cancelState = 0;
    QTimer cancelDrive;
    cancelDrive.setInterval(5);
    deadline.restart();
    QObject::connect(
        &cancelDrive, &QTimer::timeout,
        [&]
        {
            auto dialog = dynamic_cast<BatchDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            try
            {
                check(deadline.elapsed() < 45000, "Actual batch UI cancellation deadline");
                auto start = dialog->findChild<QPushButton*>("batchStart");
                auto cancel = dialog->findChild<QPushButton*>("batchCancel");
                if (cancelState == 0)
                {
                    dialog->setInputs({cancelInput, first});
                    dialog->findChild<QLineEdit*>("batchDirectory")->setText(cancelOutput);
                    QTest::mouseClick(start, Qt::LeftButton);
                    cancelState = 1;
                }
                else if (cancelState == 1 &&
                         dialog->findChild<QLabel*>("batchMessage")->text().contains("1 / 2ページ"))
                {
                    QTest::mouseClick(cancel, Qt::LeftButton);
                    cancelState = 2;
                }
                else if (cancelState == 2 && start->isEnabled() && dialog->results().size() == 2)
                {
                    const auto& rows = dialog->results();
                    check(rows[0].status == BatchStatus::Cancelled &&
                              rows[1].status == BatchStatus::NotProcessed &&
                              !QFileInfo::exists(rows[0].destination) &&
                              !QFileInfo::exists(rows[1].destination),
                          "Actual UI cancels live OCR and starts no next input");
                    cancelState = 3;
                    QTest::mouseClick(cancel, Qt::LeftButton);
                }
            }
            catch (const std::exception& failure)
            {
                error = QString::fromUtf8(failure.what());
                cancelDrive.stop();
                QMetaObject::invokeMethod(dialog, "reject", Qt::QueuedConnection);
            }
        });
    cancelDrive.start();
    window.processMultipleDocuments();
    cancelDrive.stop();
    check(error.isEmpty() && cancelState == 3,
          error.isEmpty() ? "Actual UI OCR cancellation complete" : error);
    check(window.doc.pdf() == snapshot && window.doc.cursor == cursor &&
              window.doc.revision == revision && window.doc.dirty() &&
              fileHash(window.doc.source) == sourceHash,
          "Batch dialog retains original document and history");
    window.doc.undo();
    window.doc.redo();
    check(window.doc.pdf() == snapshot, "Original document Undo/Redo retained");
    return {{"actual_order_remove_retry_process_reject_existing", true},
            {"actual_UI_live_OCR_cancel", true},
            {"compact_800x480", true},
            {"original_state_retained", true},
            {"GUI_ticks", ticks},
            {"native_UI", "未実行"}};
}
QJsonObject testBatchOcr(const QString& fixtures, const QString& output)
{
    const auto scan = readPdf(fixtures + "/D03.pdf");
    Document japanese, english;
    japanese.history = {mergeDocuments({selectPages(scan, {2}), readPdf(fixtures + "/D07.pdf")})};
    japanese.saved = -1;
    english.history = {selectPages(scan, {6})};
    english.saved = -1;
    japanese.putSignature(0, "OCR前の署名", {100, 200}, 12, Qt::blue);
    for (const auto& field : formFields(japanese.pdf()))
        if (field.qualifiedName == "name")
            putFormValue(japanese, field.widget, {"一括OCRのフォーム"});
    english.putSignature(0, "Keep before OCR", {100, 200}, 12, Qt::blue);
    const auto inputJp = output + "/batch-scan-jpn.pdf", inputEn = output + "/batch-scan-eng.pdf";
    writeCandidate(japanese.pdf(), inputJp);
    writeCandidate(english.pdf(), inputEn);
    const auto jHash = fileHash(inputJp), eHash = fileHash(inputEn);
    const auto target = folder(output, "batch-ocr-output");
    const auto plan = prepareBatch({inputJp, inputEn}, target, BatchOperation::Ocr);
    const auto rows = processBatch(plan);
    check(rows.size() == 2 && rows[0].status == BatchStatus::Saved &&
              rows[1].status == BatchStatus::Saved,
          "Actual batch Japanese and English OCR saves both files");
    auto jp = readPdf(rows[0].destination), en = readPdf(rows[1].destination);
    check(pageText(jp, 0).contains("市民公園") && pageText(en, 0).contains("coastal"),
          "Fixed bilingual OCR terms after saved output");
    bool formRetained = false;
    for (const auto& field : formFields(jp))
        if (field.qualifiedName == "name")
            formRetained = field.values == QStringList{"一括OCRのフォーム"};
    check(formRetained && renderPage(jp, 1, .6) == renderPage(japanese.pdf(), 1, .6),
          "Batch OCR preserves Japanese form values and appearances");
    check(!signatures(jp, 0).isEmpty() && !signatures(en, 0).isEmpty(),
          "Signature before OCR retained");
    auto sourceJp = japanese.pdf(), sourceEn = english.pdf();
    check(renderPage(jp, 0, .6) == renderPage(sourceJp, 0, .6) &&
              renderPage(en, 0, .6) == renderPage(sourceEn, 0, .6),
          "OCR visible content exact");
    check(fileHash(inputJp) == jHash && fileHash(inputEn) == eHash,
          "Batch OCR input hashes unchanged");
    Window reopened;
    reopened.doc.open(rows[0].destination);
    reopened.refresh(true);
    reopened.show();
    reopened.canvas->setFocus();
    QTest::keyClick(reopened.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    reopened.query->setText("市民公園");
    QTest::keyClick(reopened.query, Qt::Key_Return);
    auto search = reopened.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              15000),
          "Actual application search of saved batch OCR output");
    reopened.canvas->setZoom(.5);
    reopened.canvas->goToPage(0);
    check(QTest::qWaitFor([&] { return reopened.canvas->pageReady(0); }, 15000),
          "Saved batch OCR page ready");
    const auto pageSizePoints = pageSize(reopened.doc.pdf().getCatalog()->getPage(0));
    const auto first = reopened.canvas->mapFromScene({1, 1}),
               last = reopened.canvas->mapFromScene(
                   {pageSizePoints.width() - 1, pageSizePoints.height() - 1});
    QTest::mousePress(reopened.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(reopened.canvas->viewport(), last, 60);
    QTest::mouseRelease(reopened.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return reopened.canvas->selectionReady(); }, 15000),
          "Saved batch OCR actual selection ready");
    QTest::keyClick(reopened.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto copied = QApplication::clipboard()->text();
    check(copied.size() > 500 && copied.contains("市民公園"),
          "Actual Qt copy of saved batch OCR text");
    const auto signature = signatures(reopened.doc.pdf(), 0).first();
    reopened.doc.moveSignature(0, signature, {10, 5});
    reopened.doc.undo();
    reopened.doc.redo();
    reopened.doc.save(output + "/batch-ocr-edited.pdf");
    check(pageText(reopened.doc.pdf(), 0).contains("市民公園"),
          "Batch OCR text survives reediting and save");
    saveResults(rows, output + "/batch-ocr-results.json");
    const auto partialFolder = folder(output, "batch-partial-cancel");
    const auto partialPlan = prepareBatch({inputJp, inputEn}, partialFolder, BatchOperation::Ocr);
    bool cancelled = false;
    auto partial = processBatch(
        partialPlan, [&] { return cancelled; },
        [&](int index, const BatchItemResult& row)
        {
            if (index == 0 && row.status == BatchStatus::Saved)
                cancelled = true;
        });
    check(cancelled && partial[0].status == BatchStatus::Saved &&
              partial[1].status == BatchStatus::NotProcessed &&
              QFileInfo::exists(partial[0].destination) &&
              !QFileInfo::exists(partial[1].destination),
          "Cancellation after first success retains saved file and starts no next input");
    const auto crashFolder = folder(output, "batch-worker-crash");
    const auto crashInput = output + "/batch-crash-scan.pdf";
    writeCandidate(selectPages(scan, {2, 6}), crashInput);
    const auto crashPlan =
        prepareBatch({crashInput, fixtures + "/D01.pdf"}, crashFolder, BatchOperation::Ocr);
    bool killed = false;
    auto crashed = processBatch(crashPlan, {},
                                [&](int, const BatchItemResult& row)
                                {
                                    if (!killed && row.message.contains("1 / 2ページ"))
                                        killed = killOwnedWorker();
                                });
    check(killed && crashed[0].status == BatchStatus::Failed &&
              crashed[1].status == BatchStatus::Unneeded &&
              !QFileInfo::exists(crashed[0].destination),
          "Actual owned OCR child crash publishes no partial result and continues to next input");
    const auto cancelFolder = folder(output, "batch-ocr-active-cancel");
    const auto cancelPlan = prepareBatch({crashInput, inputEn}, cancelFolder, BatchOperation::Ocr);
    bool activeCancel = false;
    auto active = processBatch(
        cancelPlan, [&] { return activeCancel; },
        [&](int, const BatchItemResult& row)
        {
            if (row.message.contains("1 / 2ページ"))
                activeCancel = true;
        });
    check(activeCancel && active[0].status == BatchStatus::Cancelled &&
              active[1].status == BatchStatus::NotProcessed &&
              !QFileInfo::exists(active[0].destination) &&
              !QFileInfo::exists(active[1].destination),
          "Cancel actual OCR child after first page with no partial PDF");
    saveResults(partial, output + "/batch-partial-cancel-results.json");
    saveResults(crashed, output + "/batch-crash-results.json");
    return {{"actual_bilingual_OCR", true},
            {"actual_application_search_Qt_copy_reedit_save", true},
            {"copied_characters", copied.size()},
            {"saved_fixed_search_terms", true},
            {"visible_content_and_signature_retained", true},
            {"cancel_after_saved_file", true},
            {"actual_owned_worker_crash", true},
            {"actual_active_OCR_cancel", true}};
}
} // namespace tatsu
