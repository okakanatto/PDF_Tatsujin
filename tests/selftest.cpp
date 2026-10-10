#include "selftest.h"
#include "annotation_operations.h"
#include "annotation_tests.h"
#include "batch_tests.h"
#include "bookmark_edit_tests.h"
#include "certificate_signing_tests.h"
#include "certificate_tests.h"
#include "compact_viewer_tests.h"
#include "comparison_tests.h"
#include "encryption_tests.h"
#include "existing_image_tests.h"
#include "existing_text_tests.h"
#include "font_tests.h"
#include "form_data_tests.h"
#include "form_design_tests.h"
#include "form_fields.h"
#include "form_tests.h"
#include "image_decode_tests.h"
#include "image_export_tests.h"
#include "image_pdf_tests.h"
#include "layout_transition_tests.h"
#include "link_edit_tests.h"
#include "navigation_tests.h"
#include "ocr_job_tests.h"
#include "office_import_tests.h"
#include "page_crop_tests.h"
#include "page_decoration_tests.h"
#include "page_geometry_tests.h"
#include "page_operations.h"
#include "page_tests.h"
#include "pan_tests.h"
#include "pdf_objects.h"
#include "pdf_optimization_tests.h"
#include "pdfdocumentbuilder.h"
#include "reading_tests.h"
#include "redaction_copy_tests.h"
#include "redaction_scan_tests.h"
#include "reference_workflow_tests.h"
#include "save_candidate_tests.h"
#include "search_tests.h"
#include "selection_tests.h"
#include "unprotected_pdf_tests.h"
#include "viewer_tests.h"
#include "window.h"
#include "writing_tests.h"
#include <QPrinterInfo>
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
    auto temp = privateTemporaryDirectory(QDir::tempPath() + "/pdf-tatsujin-worker-test-XXXXXX");
    require(temp->isValid(), "temporary directory");
    writeCandidate(doc, temp->filePath("input.pdf"));
    QFile opt(temp->filePath("options.json"));
    opt.open(QIODevice::WriteOnly);
    opt.write(QJsonDocument(QJsonObject{{"pages", pages}, {"language", lang}}).toJson());
    opt.close();
    QProcess p;
    WorkerChannels channels(p, temp->path());
    p.start(QCoreApplication::applicationFilePath(),
            {"--ocr-worker", temp->filePath("input.pdf"), output, temp->filePath("options.json"),
             temp->filePath("report.json")});
    const bool started = p.waitForStarted();
    require(started, "worker started: " + p.errorString());
    QElapsedTimer timer;
    timer.start();
    QByteArray progress;
    int ticks = 0;
    while (p.state() != QProcess::NotRunning && timer.elapsed() < 300000)
    {
        if (channels.usesFiles())
            p.waitForFinished(30);
        else
            p.waitForReadyRead(30);
        progress += channels.progress();
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
            "OCR worker: " + channels.error());
    QFile report(temp->filePath("report.json"));
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
                                            {"qt_platform", QGuiApplication::platformName()},
                                            {"qt", qVersion()},
                                            {"platform", QSysInfo::prettyProductName()}})
                      .toJson());
    };
    auto input = [&](QString name) { return fixtures + "/" + name; };
    auto dest = [&](QString name) { return output + "/" + name; };
    run("Reading_device_image_decode", [&] { return testDeviceImageDecode(fixtures); });
    run("C07_OCR_job_cleanup", [&] { return testOcrJobCleanup(output); });
    run("C07_save_candidate_cleanup", [&] { return testSaveCandidateCleanup(fixtures, output); });
    run("C07_LongPaths", [&] { return testLongWindowsPaths(fixtures, output); });
    run("A09_OCR_result_contract", [&] { return testOcrResultValidation(fixtures, output); });
    run("A09_OCR_window_teardown", [&] { return testOcrWindowTeardown(fixtures, output); });
    run("B01_writing_roundtrip", [&] { return testWritingRoundtrip(fixtures, output); });
    run("B01_writing_coordinates", [&] { return testWritingCoordinates(fixtures, output); });
    run("B01_writing_UI", [&] { return testWritingUi(fixtures, output); });
    run("B01_fonts_roundtrip", [&] { return testFontRoundtrip(fixtures, output); });
    run("B01_fonts_failures", [&] { return testFontFailures(fixtures, output); });
    run("B01_fonts_UI", [&] { return testFontUi(fixtures, output); });
    run("B01_signature_library", [&] { return testSignatureLibrary(fixtures, output); });
    run("B04_page_arrange", [&] { return testPageArrange(fixtures, output); });
    run("B05_page_merge_insert", [&] { return testPageMerge(fixtures, output); });
    run("B06_page_exports", [&] { return testPageExports(fixtures, output); });
    run("B04_page_organizer_UI", [&] { return testPageOrganizer(fixtures, output); });
    run("B03_form_values", [&] { return testFormValues(fixtures, output); });
    run("B03_form_input_UI", [&] { return testFormInput(fixtures, output); });
    run("B03_form_keyboard", [&] { return testFormKeyboard(fixtures, output); });
    run("M2_visible_panel_lifetime", [&] { return testWindowPanelLifetime(fixtures, output); });
    run("B02_annotations", [&] { return testAnnotations(fixtures, output); });
    run("B02_annotations_UI", [&] { return testAnnotationInput(fixtures, output); });
    auto searchReady = [&](Window& window)
    {
        auto session = window.findChild<SearchPanel*>("searchPanel")->session();
        require(QTest::qWaitFor(
                    [&]
                    { return session->complete() && session->totalPages() == window.doc.pages(); },
                    15000),
                "asynchronous search finished");
    };
    run("B08_combined_workflow",
        [&]
        {
            Window window;
            QStringList dialogs;
            QTimer dismiss;
            QObject::connect(&dismiss, &QTimer::timeout,
                             [&]
                             {
                                 for (auto widget : QApplication::topLevelWidgets())
                                     if (auto box = qobject_cast<QMessageBox*>(widget))
                                     {
                                         dialogs.append(box->windowTitle());
                                         box->accept();
                                     }
                             });
            dismiss.start(25);
            window.show();
            window.openFile(input("D07.pdf"));
            const auto originalHash = fileHash(input("D07.pdf"));
            auto fields = formFields(window.doc.pdf());
            auto name = std::find_if(fields.begin(), fields.end(),
                                     [](const auto& field) { return field.name == "name"; });
            require(name != fields.end(), "input name field");
            putFormValue(window.doc, name->widget, {"髙橋 香織"});
            window.doc.putSignature(0, "山田 太郎", {350, 350}, 18, Qt::black);
            putAnnotation(window.doc, 0, OverlayKind::Rectangle, {345, 340, 130, 45}, "署名を確認",
                          Qt::blue, 1.5);
            auto scan = readPdf(input("D05.pdf"));
            window.doc.commit(insertPages(window.doc.pdf(), scan, {0}, 1));
            window.doc.rotate(1);
            window.refresh(true);
            const auto beforeOcr = window.doc.pdf();
            writeCandidate(beforeOcr, dest("m2-combined-before-OCR.pdf"));
            window.scope->setCurrentIndex(0);
            window.startOcr();
            require(window.doc.busy, "same-window OCR starts after form and page edits");
            require(QTest::qWaitFor([&] { return !window.worker; }, 180000),
                    "combined OCR completes");
            require(!window.doc.busy && window.doc.pdf() != beforeOcr,
                    "completed OCR is committed");
            require(dialogs == QStringList{"OCR結果"}, "combined OCR has only its result dialog");
            window.undoAction->trigger();
            require(window.doc.pdf() == beforeOcr, "OCR Undo preserves all preceding edits");
            window.redoAction->trigger();
            const auto ocr = window.doc.pdf();
            writeCandidate(ocr, dest("m2-combined-after-OCR.pdf"));
            window.doc.commit(selectPages(window.doc.pdf(), {1, 0}));
            window.refresh(true);
            require(pageText(window.doc.pdf(), 0).contains("図書館"),
                    "OCR text follows reordered page");
            window.doc.undo();
            require(window.doc.pdf() == ocr, "page reorder Undo retains form and OCR");
            window.doc.redo();
            window.refresh(true);
            window.query->setText("図書館");
            QTest::keyClick(window.query, Qt::Key_Return);
            searchReady(window);
            require(window.findChild<SearchPanel*>("searchPanel")->session()->rowCount() == 1,
                    "search works after combined page edits and OCR");
            auto savedFields = formFields(window.doc.pdf());
            writeCandidate(window.doc.pdf(), dest("m2-combined-after-reorder.pdf"));
            QJsonArray fieldReport;
            for (const auto& field : savedFields)
                fieldReport.append(
                    QJsonObject{{"page", field.page},
                                {"name", field.name},
                                {"values", QJsonArray::fromStringList(field.values)}});
            QFile fieldFile(dest("m2-combined-fields.json"));
            require(fieldFile.open(QIODevice::WriteOnly), "combined field diagnostics");
            fieldFile.write(QJsonDocument(fieldReport).toJson());
            fieldFile.close();
            require(std::any_of(savedFields.begin(), savedFields.end(),
                                [](const auto& field) {
                                    return field.page == 1 && field.name == "name" &&
                                           field.values == QStringList{"髙橋 香織"};
                                }),
                    "Japanese form follows its page");
            require(signatures(window.doc.pdf(), 1).size() == 2,
                    "signature and annotation follow original form page");
            window.doc.save(dest("m2-combined.pdf"));
            const auto rendered = renderPage(window.doc.pdf(), 1, 1.2);
            window.doc.undo();
            require(window.doc.dirty(), "Undo after save marks document unsaved");
            window.doc.redo();
            require(!window.doc.dirty(), "Redo to saved state clears unsaved indicator");
            Document reopened;
            reopened.open(dest("m2-combined.pdf"));
            require(renderPage(reopened.pdf(), 1, 1.2) == rendered,
                    "combined saved appearance retained");
            reopened.moveSignature(1, signatures(reopened.pdf(), 1).front(), {3, 4});
            bool refused = false;
            try
            {
                reopened.save(dest("missing-folder/m2-combined.pdf"));
            }
            catch (const std::exception&)
            {
                refused = true;
            }
            require(refused && reopened.dirty(), "failed save retains all edits");
            reopened.save(dest("m2-combined-reedited.pdf"));
            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(dest("m2-combined-print.pdf"));
            printDocument(window.doc.pdf(), printer);
            require(readPdf(dest("m2-combined-print.pdf")).getCatalog()->getPageCount() == 2,
                    "combined document prints both pages");
            require(fileHash(input("D07.pdf")) == originalHash,
                    "combined workflow protects source");
            return QJsonObject{{"same_window", true},
                               {"form_signature_annotation_pages_OCR", true},
                               {"search_save_reedit_print", true},
                               {"save_retry", true}};
        });
    run("Navigation_bookmarks", [&] { return testNavigationBookmarks(fixtures, output); });
    run("Navigation_links", [&] { return testNavigationLinks(fixtures, output); });
    run("Navigation_lifecycle", [&] { return testNavigationLifecycle(fixtures, output); });
    run("Navigation_history", [&] { return testViewHistory(); });
    run("Reading_page_input", [&] { return testReadingPageInput(fixtures, output); });
    run("Reading_focus_layout", [&] { return testReadingLayout(fixtures, output); });
    run("Reading_OCR_cancel", [&] { return testReadingOcrCancel(fixtures, output); });
    run("Reading_initial_panels", [&] { return testReadingInitialPanels(fixtures, output); });
    run("Reading_compiler_startup", [&] { return testReadingCompilerStartup(fixtures, output); });
    run("Reading_image_navigation", [&] { return testReadingImageNavigation(fixtures, output); });
    run("Reading_shared_icons", [&] { return testReadingIcons(); });
    run("Reading_compact_layout", [&] { return testCompactViewer(fixtures, output); });
    run("Reading_compact_OCR", [&] { return testCompactOcr(fixtures, output); });
    run("Reading_layout_transitions", [&] { return testLayoutTransitions(fixtures, output); });
    run("Reading_resize_navigation_input",
        [&] { return testResizeNavigationInput(fixtures, output); });
    run("Reading_automatic_fit", [&] { return testAutomaticFit(fixtures, output); });
    run("Reference_compact", [&] { return testCompactReferences(fixtures, output); });
    run("Reference_repeat_search", [&] { return testRepeatSearchReference(fixtures, output); });
    run("Reference_result_resize", [&] { return testSearchResultResize(fixtures, output); });
    run("Reference_OCR_reading", [&] { return testReferenceDuringOcr(fixtures, output); });
    run("Reference_contexts", [&] { return testReferenceContexts(fixtures, output); });
    run("M4I01_image_pdf_geometry", [&] { return testImagePdfGeometry(output); });
    run("M4I02_image_pdf_failures", [&] { return testImagePdfFailures(output); });
    run("M4I03_image_pdf_window", [&] { return testImagePdfWindow(fixtures, output); });
    run("M4I04_image_pdf_cancel_retry", [&] { return testImagePdfCancelRetry(output); });
    run("M4I05_image_pdf_OCR", [&] { return testImagePdfOcr(fixtures, output); });
    run("M4E01_image_export_geometry", [&] { return testImageExportGeometry(fixtures, output); });
    run("M4E02_image_export_failures", [&] { return testImageExportFailures(fixtures, output); });
    run("M4E03_image_export_UI", [&] { return testImageExportUi(fixtures, output); });
    run("Geometry01_page_axes", [&] { return testPageAxes(fixtures); });
    run("Geometry02_page_render", [&] { return testPageGeometryRender(fixtures, output); });
    run("M4C01_page_crop_geometry", [&] { return testPageCropGeometry(fixtures, output); });
    run("M4C02_page_crop_failures", [&] { return testPageCropFailures(fixtures); });
    run("M4C03_page_crop_UI", [&] { return testPageCropUi(fixtures, output); });
    run("M4D01_decoration_lifecycle", [&] { return testDecorationLifecycle(fixtures, output); });
    run("M4D02_decoration_failures", [&] { return testDecorationFailures(fixtures); });
    run("M4D03_decoration_UI", [&] { return testDecorationUi(fixtures, output); });
    run("M4D04_decoration_cancel", [&] { return testDecorationCancel(fixtures); });
    run("M4D05_decoration_OCR", [&] { return testDecorationOcr(fixtures, output); });
    run("M4B01_bookmark_lifecycle", [&] { return testBookmarkLifecycle(fixtures, output); });
    run("M4B02_bookmark_failures", [&] { return testBookmarkFailures(fixtures); });
    run("M4B03_bookmark_UI", [&] { return testBookmarkEditUi(fixtures, output); });
    run("M4B04_bookmark_cancel", [&] { return testBookmarkCancel(fixtures); });
    run("M4L01_link_lifecycle", [&] { return testLinkLifecycle(fixtures, output); });
    run("M4L02_link_failures", [&] { return testLinkFailures(fixtures); });
    run("M4L03_link_UI", [&] { return testLinkUi(fixtures, output); });
    run("M4L04_link_OCR", [&] { return testLinkOcr(fixtures, output); });
    run("M4O01_optimization_lifecycle",
        [&] { return testOptimizationLifecycle(fixtures, output); });
    run("M4O02_optimization_failures", [&] { return testOptimizationFailures(fixtures); });
    run("M4O03_optimization_UI", [&] { return testOptimizationUi(fixtures, output); });
    run("M4O04_optimization_OCR", [&] { return testOptimizationOcr(fixtures, output); });
    run("M5P01_password_preparation", [&] { return testEncryptionPasswords(output); });
    run("M5P02_encryption_lifecycle", [&] { return testEncryptionLifecycle(fixtures, output); });
    run("M5P03_encryption_failures", [&] { return testEncryptionFailures(fixtures, output); });
    run("M5P04_encryption_UI", [&] { return testEncryptionUi(fixtures, output); });
    run("M5P05_encryption_OCR", [&] { return testEncryptionOcr(fixtures, output); });
    run("M5F01_form_data_lifecycle", [&] { return testFormDataLifecycle(fixtures, output); });
    run("M5F02_form_data_failures", [&] { return testFormDataFailures(fixtures, output); });
    run("M5F03_form_data_UI", [&] { return testFormDataUi(fixtures, output); });
    run("M5F04_form_data_OCR", [&] { return testFormDataOcr(fixtures, output); });
    run("M5U01_unprotected_lifecycle", [&] { return testUnprotectedLifecycle(fixtures, output); });
    run("M5U02_unprotected_failures", [&] { return testUnprotectedFailures(fixtures, output); });
    run("M5U03_unprotected_UI", [&] { return testUnprotectedUi(fixtures, output); });
    run("M5U04_unprotected_OCR", [&] { return testUnprotectedOcr(fixtures, output); });
    run("M5C01_comparison_lifecycle", [&] { return testComparisonLifecycle(fixtures, output); });
    run("M5C02_comparison_failures", [&] { return testComparisonFailures(fixtures, output); });
    run("M5C03_comparison_UI", [&] { return testComparisonUi(fixtures, output); });
    run("M5C04_comparison_OCR", [&] { return testComparisonOcr(fixtures, output); });
    run("M5T01_batch_lifecycle", [&] { return testBatchLifecycle(fixtures, output); });
    run("M5T02_batch_failures", [&] { return testBatchFailures(fixtures, output); });
    run("M5T03_batch_UI", [&] { return testBatchUi(fixtures, output); });
    run("M5T04_batch_OCR", [&] { return testBatchOcr(fixtures, output); });
    run("M5D01_form_design_lifecycle", [&] { return testFormDesignLifecycle(fixtures, output); });
    run("M5D02_form_design_failures", [&] { return testFormDesignFailures(fixtures, output); });
    run("M5D03_form_design_UI", [&] { return testFormDesignUi(fixtures, output); });
    run("M5D04_form_design_OCR", [&] { return testFormDesignOcr(fixtures, output); });
    run("M5S01_certificate_cases", [&] { return testCertificateCases(fixtures, output); });
    run("M5S02_certificate_guards", [&] { return testCertificateGuards(fixtures, output); });
    run("M5S03_certificate_UI", [&] { return testCertificateUi(fixtures, output); });
    run("M5S04_certificate_signing_foundation",
        [&] { return testCertificateSigningFoundation(fixtures, output); });
    run("M5S05_certificate_signing_UI", [&] { return testCertificateSigningUi(fixtures, output); });
    run("M5S06_certificate_signing_atomic",
        [&] { return testCertificateSigningAtomic(fixtures, output); });
    run("M5S07_certificate_signing_OCR",
        [&] { return testCertificateSigningOcr(fixtures, output); });
    run("M5R01_redaction_foundation",
        [&] { return testRedactionCopyFoundation(fixtures, output); });
    run("M5R02_redaction_atomic", [&] { return testRedactionCopyAtomic(fixtures, output); });
    run("M5R03_redaction_UI", [&] { return testRedactionCopyUi(fixtures, output); });
    run("M5R04_redaction_font_UI", [&] { return testRedactionCopyFontUi(fixtures, output); });
    run("M5R06_redaction_real_OCR", [&] { return testRedactionCopyOcr(fixtures, output); });
    run("M5R07_redaction_scan_then_OCR", [&] { return testRedactionScanOcr(fixtures, output); });
    run("M5R08_redaction_OCR_mask_preservation", [&] { return testRedactionOcrMasks(); });
    run("M6I01_existing_image_core", [&] { return testExistingImageCore(fixtures, output); });
    run("M6IF01_existing_form_images", [&] { return testExistingFormImages(fixtures, output); });
    run("M6IF02_existing_form_image_UI", [&] { return testExistingFormImageUi(fixtures, output); });
    run("M6T01_existing_text_core", [&] { return testExistingTextCore(fixtures, output); });
    run("M6T02_existing_text_refusals", [&] { return testExistingTextRefusals(fixtures, output); });
    run("M6T03_existing_text_UI", [&] { return testExistingTextUi(fixtures, output); });
    run("M6T04_existing_text_UI_refusals",
        [&] { return testExistingTextUiRefusals(fixtures, output); });
    run("M6T05_existing_text_fonts", [&] { return testExistingTextFonts(fixtures, output); });
    run("M6T06_existing_text_font_UI", [&] { return testExistingTextFontUi(fixtures, output); });
    run("M6T07_existing_text_multiline",
        [&] { return testExistingTextMultiline(fixtures, output); });
    run("M6T08_existing_text_multiline_UI",
        [&] { return testExistingTextMultilineUi(fixtures, output); });
    run("M6T09_existing_text_relative_lines",
        [&] { return testExistingTextRelativeLines(fixtures, output); });
    run("M6T10_existing_text_line_count",
        [&] { return testExistingTextLineCount(fixtures, output); });
    run("M6T11_existing_text_line_count_UI",
        [&] { return testExistingTextLineCountUi(fixtures, output); });
    run("M6TF01_existing_form_text", [&] { return testExistingFormText(fixtures, output); });
    run("M6TF02_existing_form_text_UI", [&] { return testExistingFormTextUi(fixtures, output); });
    run("M6TW01_existing_text_wrap", [&] { return testExistingTextWrap(fixtures, output); });
    run("M6TW02_existing_text_wrap_UI", [&] { return testExistingTextWrapUi(fixtures, output); });
    run("M6O01_DOCX_conversion", [&] { return testOfficeImport(fixtures, output); });
    run("M6O02_DOCX_UI", [&] { return testOfficeImportUi(fixtures, output); });
    run("M6O03_owned_process", [&] { return testOwnedProcess(output); });
    run("M6O04_DOCX_new_window", [&] { return testOfficeImportWindow(fixtures, output); });
    run("M6I03_existing_image_UI", [&] { return testExistingImageUi(fixtures, output); });
    run("M6I04_existing_image_UI_modes",
        [&] { return testExistingImageUiModes(fixtures, output); });
    run("M6I02_existing_image_refusals",
        [&] { return testExistingImageRejections(fixtures, output); });
    run("M5R05_redaction_geometry_UI",
        [&] { return testRedactionCopyGeometryUi(fixtures, output); });
    run("Pan_navigation", [&] { return testPanNavigation(fixtures, output); });
    run("Pan_input", [&] { return testPanInput(fixtures, output); });
    run("Pan_lifecycle", [&] { return testPanLifecycle(fixtures, output); });
    run("Selection_ranges", [&] { return testSelectionRanges(fixtures, output); });
    run("Selection_autoscroll", [&] { return testSelectionScroll(fixtures, output); });
    run("Selection_lifecycle", [&] { return testSelectionLifecycle(fixtures, output); });
    run("Selection_words", [&] { return testSelectionWords(fixtures, output); });
    run("Selection_word_boundaries", [&] { return testSelectionWordBoundaries(fixtures, output); });
    run("Selection_word_lifecycle", [&] { return testSelectionWordLifecycle(fixtures, output); });
    run("Viewer_page_previews", [&] { return testPagePreviews(fixtures, output); });
    run("Search_occurrences_history", [&] { return testSearchNavigation(fixtures, output); });
    run("Search_generation_lifecycle", [&] { return testSearchGeneration(fixtures, output); });
    run("Search_IME_commit", [&] { return testSearchInput(fixtures, output); });
    run("Viewer_continuous_navigation", [&] { return testViewerNavigation(fixtures, output); });
    run("Viewer_crop_rotation_userunit", [&] { return testViewerCoordinates(fixtures, output); });
    run("Viewer_signature_save_undo", [&] { return testViewerSignature(fixtures, output); });
    if (qEnvironmentVariableIsSet("TATSU_UI_REVIEW"))
        run("A01_UI_layout_review",
            [&]
            {
                Window w;
                w.resize(1024, 720);
                w.show();
                QTest::qWait(150);
                w.grab().save(dest("welcome-1024.png"));
                w.openFile(input("D01.pdf"));
                w.signatureAction->trigger();
                w.signature->setPlainText("山田 太郎\n髙橋\n2026年10月4日");
                QTest::qWait(150);
                w.grab().save(dest("signature-1024.png"));
                auto editor = w.size->findChild<QLineEdit*>();
                for (auto widget : w.panels->currentWidget()->findChildren<QWidget*>())
                {
                    if (qobject_cast<QPushButton*>(widget) ||
                        qobject_cast<QAbstractSpinBox*>(widget))
                    {
                        const auto rect = QRect(widget->mapTo(&w, QPoint()), widget->size());
                        require(widget->isVisible() && w.rect().contains(rect),
                                "signature controls fit the minimum window");
                    }
                }
                QJsonObject metrics{{"size_text", w.size->text()},
                                    {"size_font", w.size->font().toString()},
                                    {"editor_font", editor->font().toString()},
                                    {"editor_text", editor->text()},
                                    {"device_pixel_ratio", w.devicePixelRatioF()}};
                w.ocrAction->trigger();
                QTest::qWait(150);
                w.grab().save(dest("ocr-settings-1024.png"));
                return metrics;
            });
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
    run("A01_search_highlight_lifecycle",
        [&]
        {
            Window w;
            w.openFile(input("D01.pdf"));
            w.show();
            QTest::qWait(100);
            w.query->setFocus();
            const auto plain = w.canvas->viewport()->grab().toImage();
            w.query->setText("English");
            QTest::keyClick(w.query, Qt::Key_Return);
            searchReady(w);
            const auto highlighted = w.canvas->viewport()->grab().toImage();
            require(highlighted != plain, "matching text is visibly highlighted");
            w.query->setText("no-such-text-92817");
            QTest::keyClick(w.query, Qt::Key_Return);
            searchReady(w);
            require(w.canvas->viewport()->grab().toImage() == plain,
                    "zero results remove the previous query's highlight");
            w.query->setText("English");
            QTest::keyClick(w.query, Qt::Key_Return);
            searchReady(w);
            QTest::keyClick(w.query, Qt::Key_Return);
            searchReady(w);
            require(w.canvas->viewport()->grab().toImage() == highlighted,
                    "repeating a search does not stack highlights");
            w.refresh();
            require(w.canvas->viewport()->grab().toImage() == highlighted,
                    "document refresh preserves the active query's highlight");
            w.query->clear();
            require(w.canvas->viewport()->grab().toImage() == plain &&
                        !w.pages->item(0)->text().contains("一致") &&
                        !w.status->text().contains("一致"),
                    "clearing the query clears highlights and match summaries");
            return QJsonObject{{"event_source", "Qt QTest synthetic key events"},
                               {"zero_results_cleared", true},
                               {"refresh_preserves_highlights", true}};
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
            const auto expectedDelta = matrix.inverted().map(w.canvas->mapToScene(end)) -
                                       matrix.inverted().map(w.canvas->mapToScene(start));
            int cursor = w.doc.cursor;
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(w.canvas->viewport(), end, 30);
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, end);
            require(w.doc.cursor == cursor + 1, "one drag one undo");
            const auto after = signatures(w.doc.pdf(), 0).at(0);
            require(QLineF(after.rect.topLeft(), before.rect.topLeft() + expectedDelta).length() <
                        .5,
                    "opening signature panel does not shift drag coordinates");
            w.undoAction->trigger();
            require(signatures(w.doc.pdf(), 0).at(0).rect == before.rect, "toolbar undo");
            w.redoAction->trigger();
            require(signatures(w.doc.pdf(), 0).at(0).rect != before.rect, "toolbar redo");
            auto a = w.canvas->mapFromScene(QPointF(45, 55));
            auto b = w.canvas->mapFromScene(QPointF(545, 108));
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, a);
            QTest::mouseMove(w.canvas->viewport(), b, 20);
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, b);
            require(QTest::qWaitFor([&] { return w.canvas->selectionReady(); }, 15000),
                    "selected text finishes loading");
            QTest::keyClick(w.canvas, Qt::Key_C, Qt::ControlModifier);
            require(QApplication::clipboard()->text().contains("English"),
                    "selection copy clipboard");
            w.grab().save(dest("signature-ui.png"));
            w.doc.saved = w.doc.cursor;
            return QJsonObject{{"event_source", "Qt QTest synthetic mouse events"},
                               {"clipboard_copy", true},
                               {"IME", "未実行・確定済み文字列を使用"}};
        });
    run("A02_UI_placement_cancel",
        [&]
        {
            Window w;
            w.openFile(input("D01.pdf"));
            w.show();
            w.signatureAction->trigger();
            w.signature->setPlainText("山田 太郎");
            QTest::qWait(100);
            QPushButton* placeButton = nullptr;
            for (auto b : w.findChildren<QPushButton*>())
                if (b->text() == "ページをクリックして配置")
                    placeButton = b;
            require(placeButton, "placement button exists");
            QTest::mouseClick(placeButton, Qt::LeftButton);
            require(w.canvas->placing, "placement starts through the actual button");
            auto focused = QApplication::focusWidget();
            require(focused, "placement has keyboard focus");
            QTest::keyClick(focused, Qt::Key_Escape);
            require(!w.canvas->placing && w.canvas->cursor().shape() == Qt::ArrowCursor &&
                        !w.status->text().contains("配置位置"),
                    "Escape cancels placement from the focus left by the placement button");
            require(!w.doc.dirty() && signatures(w.doc.pdf(), 0).isEmpty(),
                    "cancelled placement leaves the document unchanged");
            QTest::mouseClick(placeButton, Qt::LeftButton);
            w.ocrAction->trigger();
            require(!w.canvas->placing && !w.status->text().contains("配置位置"),
                    "switching to OCR cancels pending signature placement");
            return QJsonObject{{"cancelled_from_button_focus", true},
                               {"switching_tools_cancels_placement", true},
                               {"event_source", "Qt QTest synthetic mouse and key events"}};
        });
    run("A02_A03_UI_signature_identity",
        [&]
        {
            Window w;
            w.openFile(input("D01.pdf"));
            const auto first = w.doc.putSignature(0, "山田 太郎", {80, 400}, 20, Qt::black);
            const auto second = w.doc.putSignature(0, "髙橋", {80, 300}, 20, Qt::black);
            w.refresh();
            w.show();
            QTest::qWait(100);
            const auto m = pageMatrix(w.doc.pdf().getCatalog()->getPage(0));
            QTest::mouseClick(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                              w.canvas->mapFromScene(m.map(first.rect.center())));
            QPushButton* updateButton = nullptr;
            for (auto b : w.findChildren<QPushButton*>())
                if (b->text() == "選択した署名を更新")
                    updateButton = b;
            require(updateButton, "update button exists");
            for (const auto& text : {QString("山田 次郎"), QString("山田 三郎")})
            {
                w.signature->setPlainText(text);
                QTest::mouseClick(updateButton, Qt::LeftButton);
                auto all = signatures(w.doc.pdf(), 0);
                require(w.canvas->selected >= 0 && w.canvas->selected < all.size() &&
                            all[w.canvas->selected].text == text,
                        "repeated edits keep the same signature selected after annotation order "
                        "changes");
                require(std::any_of(all.begin(), all.end(),
                                    [&](const auto& s) {
                                        return s.ref == second.ref && s.text == second.text &&
                                               s.rect == second.rect;
                                    }),
                        "editing the first signature never changes the second");
            }
            w.doc.save(dest("two-signatures.pdf"));
            w.refresh();
            w.grab().save(dest("two-signatures-ui.png"));
            Document reopened;
            reopened.open(dest("two-signatures.pdf"));
            auto all = signatures(reopened.pdf(), 0);
            require(all.size() == 2 &&
                        std::any_of(all.begin(), all.end(),
                                    [](const auto& s) { return s.text == "山田 三郎"; }) &&
                        std::any_of(all.begin(), all.end(),
                                    [](const auto& s) { return s.text == "髙橋"; }),
                    "both distinct signatures survive saving and reopening");
            return QJsonObject{{"repeated_updates", 2}, {"saved_signatures", 2}};
        });
    run("A02_UI_drag_interrupted",
        [&]
        {
            Window w;
            w.openFile(input("D01.pdf"));
            const auto s = w.doc.putSignature(0, "山田 太郎", {80, 400}, 20, Qt::black);
            w.refresh();
            w.show();
            QTest::qWait(100);
            auto start = w.canvas->mapFromScene(
                pageMatrix(w.doc.pdf().getCatalog()->getPage(0)).map(s.rect.center()));
            const auto before = w.doc.pdf();
            const auto revision = w.doc.revision;
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
            w.refresh();
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                                start + QPoint(30, 20));
            require(w.doc.pdf() == before && w.doc.revision == revision,
                    "refresh cancels an in-flight drag without committing a stale movement");
            for (bool escape : {true, false})
            {
                start = w.canvas->mapFromScene(
                    pageMatrix(w.doc.pdf().getCatalog()->getPage(0)).map(s.rect.center()));
                QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(w.canvas->viewport(), start + QPoint(20, 10));
                if (escape)
                    QTest::keyClick(w.canvas, Qt::Key_Escape);
                else
                    w.canvas->setZoom(.75);
                QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                                    start + QPoint(30, 20));
                require(w.doc.pdf() == before && w.doc.revision == revision,
                        escape ? "Escape cancels a drag without adding history"
                               : "zoom during a drag cancels the obsolete coordinates");
            }
            start = w.canvas->mapFromScene(
                pageMatrix(w.doc.pdf().getCatalog()->getPage(0)).map(s.rect.center()));
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
            w.undoAction->trigger();
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                                start + QPoint(30, 20));
            require(signatures(w.doc.pdf(), 0).isEmpty() && w.doc.cursor == 0,
                    "Undo during a drag removes the signature without a stale release or crash");
            return QJsonObject{{"refresh_cancels_drag", true},
                               {"undo_during_drag", true},
                               {"escape_cancels_drag", true},
                               {"zoom_cancels_drag", true}};
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
            result["cross_page_selection"] = testSelectionOcr(dest("D03-ocr.pdf"), output);
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
            w.query->setText("図書館");
            QTest::keyClick(w.query, Qt::Key_Return);
            searchReady(w);
            require(w.findChild<SearchPanel*>("searchPanel")->session()->rowCount() == 0,
                    "Japanese scan text is not searchable before OCR");
            w.startOcr();
            require(w.doc.busy && w.cancel->isVisible(), "OCR busy state and cancel UI");
            QElapsedTimer timer;
            timer.start();
            while (w.worker && timer.elapsed() < 180000)
                QTest::qWait(20);
            require(!w.worker && !w.doc.busy, "UI OCR completion");
            require(w.doc.cursor == 2, "one OCR history transaction");
            searchReady(w);
            require(w.findChild<SearchPanel*>("searchPanel")->session()->rowCount() == 1,
                    "OCR completion refreshes the existing Japanese query without retyping");
            w.query->setText("DIGITAL HEADING");
            QTest::keyClick(w.query, Qt::Key_Return);
            searchReady(w);
            require(w.findChild<SearchPanel*>("searchPanel")->session()->rowCount() == 1,
                    "search UI shows the one DIGITAL HEADING occurrence");
            w.canvas->setZoom(.5);
            QTest::qWait(50);
            auto dims = pageSize(w.doc.pdf().getCatalog()->getPage(0));
            auto first = w.canvas->mapFromScene(QPointF(1, 1));
            auto last = w.canvas->mapFromScene(QPointF(dims.width() - 1, dims.height() - 1));
            QTest::mousePress(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
            QTest::mouseMove(w.canvas->viewport(), last, 20);
            QTest::mouseRelease(w.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
            require(QTest::qWaitFor([&] { return w.canvas->selectionReady(); }, 15000),
                    "selected OCR text finishes loading");
            QTest::keyClick(w.canvas, Qt::Key_C, Qt::ControlModifier);
            auto copied = QApplication::clipboard()->text();
            require(copied.size() > 300, "OCR clipboard text");
            w.doc.save(dest("same-window.pdf"));
            w.refresh();
            require(QTest::qWaitFor([&] { return w.canvas->pageReady(0); }, 10000),
                    "saved OCR page finishes asynchronous rendering");
            w.grab().save(dest("ocr-window.png"));
            w.undoAction->trigger();
            require(w.doc.pdf() == before, "UI undo retains signature before OCR");
            require(w.progress->text() == "元に戻しました。",
                    "OCR Undo replaces the obsolete completion message");
            w.redoAction->trigger();
            require(signatures(w.doc.pdf(), 0).size() == 1, "UI redo signature retained");
            require(w.progress->text() == "やり直しました。",
                    "OCR Redo reports the current history action");
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
    run("A10_save_conflict_path_case",
        [&]
        {
            QTemporaryDir folder;
            require(folder.isValid(), "test directory exists");
            int checked = 0;
            for (bool existingTarget : {false, true})
            {
                const auto source =
                    folder.filePath(existingTarget ? "target-source.pdf" : "source.pdf");
                require(QFile::copy(input("D01.pdf"), source), "copy test input");
                Document d;
                d.open(source);
                const auto path = existingTarget ? folder.filePath("saved.pdf") : source;
                if (existingTarget)
                    d.save(path);
                QFile original(path);
                require(original.open(QIODevice::ReadOnly), "read baseline bytes");
                const auto baselineBytes = original.readAll();
                original.close();
                d.putSignature(0, "山田 太郎", {80, 400}, 20, Qt::black);
                const auto before = d.pdf();
                const auto revision = d.revision;
                QFile external(path);
                require(external.open(QIODevice::Append), "external writer opened");
                require(external.write("\n% external change\n") > 0, "external writer changed PDF");
                external.close();
                const auto changedHash = fileHash(path);
                const auto alias = path.toUpper();
                require(alias != path && fileHash(alias) == changedHash,
                        "Windows case variant addresses the same existing file");
                bool rejected = false;
                try
                {
                    // The UI supplies the hash observed at the save dialog. A
                    // known file must instead use its open/last-save baseline.
                    d.save(alias, fileHash(alias));
                }
                catch (const std::exception&)
                {
                    rejected = true;
                }
                require(rejected && fileHash(path) == changedHash && d.pdf() == before &&
                            d.revision == revision && d.dirty(),
                        existingTarget ? "case variant preserves externally changed save target"
                                       : "case variant preserves externally changed source");
                // Restore only this test-owned file, then prove that ordinary
                // saves through either spelling still work and update baselines.
                require(external.open(QIODevice::WriteOnly | QIODevice::Truncate),
                        "restore test baseline");
                require(external.write(baselineBytes) == baselineBytes.size(),
                        "restore complete baseline");
                external.close();
                d.save(alias, fileHash(alias));
                require(!d.dirty(), "unchanged case variant can be saved");
                d.rotate(0);
                d.save(path, fileHash(path));
                require(!d.dirty(), "later save through original spelling uses updated baseline");
                ++checked;
            }
            return QJsonObject{{"protected_case_variants", checked}};
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
    run("A10_save_dialog_cancel",
        [&]
        {
            const bool previous = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            const auto restore = qScopeGuard(
                [&] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); });
            Window w;
            w.openFile(input("D01.pdf"));
            w.doc.putSignature(0, "取消の確認", {70, 400}, 18, Qt::black);
            w.refresh(true);
            const auto before = w.doc.pdf();
            const auto revision = w.doc.revision;
            const auto originalHash = fileHash(w.doc.source);
            QTimer dismiss;
            bool observed = false;
            QObject::connect(&dismiss, &QTimer::timeout,
                             [&]
                             {
                                 for (auto dialog : w.findChildren<QFileDialog*>())
                                 {
                                     if (dialog->isVisible())
                                     {
                                         observed = true;
                                         dialog->reject();
                                     }
                                 }
                             });
            dismiss.start(20);
            const bool saved = w.saveFile(true);
            dismiss.stop();
            require(observed && !saved, "actual application save dialog was cancelled");
            require(w.doc.pdf() == before && w.doc.revision == revision && w.doc.dirty(),
                    "cancel preserves document, revision and dirty state");
            require(w.doc.target.isEmpty() && fileHash(w.doc.source) == originalHash,
                    "cancel does not set a destination or change the original");
            w.doc.saved = w.doc.cursor;
            return QJsonObject{{"dialog_backend", "Qt QFileDialog (non-native)"},
                               {"dirty_and_original_preserved", true},
                               {"native_Windows_dialog", "未実行"}};
        });
    run("A10_UI_close_unsaved",
        [&]
        {
            const bool previous = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            const auto restore = qScopeGuard(
                [&] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); });
            QMessageBox::StandardButton decision = QMessageBox::Cancel;
            bool cancelSave = true;
            QString savePath;
            QString dialogFailure;
            QStringList dialogs;
            QTimer answer;
            QObject::connect(
                &answer, &QTimer::timeout,
                [&]
                {
                    auto modal = QApplication::activeModalWidget();
                    if (auto box = qobject_cast<QMessageBox*>(modal))
                    {
                        dialogs << box->windowTitle();
                        auto button = box->button(
                            box->standardButtons().testFlag(decision) ? decision : QMessageBox::Ok);
                        if (button)
                            QTest::mouseClick(button, Qt::LeftButton);
                    }
                    else if (auto dialog = qobject_cast<QFileDialog*>(modal))
                    {
                        dialogs << dialog->windowTitle();
                        if (cancelSave)
                            dialog->reject();
                        else
                        {
                            // QFileDialog::selectFile deliberately leaves a
                            // focused filename editor unchanged. Enter the
                            // requested path in that actual editor instead.
                            auto filename = dialog->findChild<QLineEdit*>("fileNameEdit");
                            if (!filename)
                            {
                                dialogFailure = "filename editor missing";
                                dialog->reject();
                                return;
                            }
                            filename->setText(QDir::toNativeSeparators(savePath));
                            const auto selected = dialog->selectedFiles();
                            if (selected.size() != 1 || !sameFilePath(selected[0], savePath))
                            {
                                dialogFailure = "dialog did not select the requested destination";
                                dialog->reject();
                                return;
                            }
                            static_cast<QDialog*>(dialog)->accept();
                        }
                    }
                });
            answer.start(20);
            Window w;
            w.openFile(input("D01.pdf"));
            w.doc.putSignature(0, "閉じる前の署名", {80, 400}, 20, Qt::black);
            w.refresh();
            w.show();
            const auto before = w.doc.pdf();
            const auto revision = w.doc.revision;
            const auto hash = fileHash(w.doc.source);
            require(!w.close() && w.isVisible(), "cancel keeps the document window open");
            decision = QMessageBox::Save;
            require(!w.close() && w.isVisible(), "cancelling Save As also cancels closing");
            require(w.doc.pdf() == before && w.doc.revision == revision && w.doc.dirty() &&
                        fileHash(w.doc.source) == hash,
                    "both cancellation routes retain the original and unsaved work");
            cancelSave = false;
            savePath = dest("close-saved.pdf");
            require(w.close() && dialogFailure.isEmpty() && !w.doc.dirty() && !w.isVisible() &&
                        sameFilePath(w.doc.target, savePath) && QFileInfo::exists(savePath),
                    "successful save to the requested destination permits closing: " +
                        dialogFailure);
            Document reopened;
            reopened.open(savePath);
            require(signatures(reopened.pdf(), 0).at(0).text == "閉じる前の署名",
                    "the saved signature is present after closing");
            Window conflict;
            conflict.openFile(input("D01.pdf"));
            conflict.doc.save(dest("close-conflict.pdf"));
            conflict.doc.putSignature(0, "失敗しても保持", {80, 400}, 20, Qt::black);
            conflict.refresh();
            conflict.show();
            QFile external(conflict.doc.target);
            require(external.open(QIODevice::Append), "open test-owned conflict file");
            external.write("\n% external change\n");
            external.close();
            const auto conflictHash = fileHash(conflict.doc.target);
            const auto unsaved = conflict.doc.pdf();
            require(!conflict.close() && conflict.isVisible() && conflict.doc.dirty() &&
                        conflict.doc.pdf() == unsaved &&
                        fileHash(conflict.doc.target) == conflictHash,
                    "save failure keeps the window, edits and external file intact");
            decision = QMessageBox::Discard;
            require(conflict.close() && !conflict.isVisible() &&
                        fileHash(conflict.doc.target) == conflictHash &&
                        fileHash(w.doc.source) == hash,
                    "explicit discard closes without writing any PDF");
            answer.stop();
            return QJsonObject{{"cancel_close", true},
                               {"cancel_save", true},
                               {"save_then_close", true},
                               {"failed_save_keeps_open", true},
                               {"explicit_discard", true},
                               {"dialogs", QJsonArray::fromStringList(dialogs)},
                               {"native_Windows_dialog", "未実行; Qt dialogs and QTest"}};
        });
    if (!qEnvironmentVariableIsEmpty("TATSU_DENIED_SAVE_DIR"))
        run("A10_NTFS_access_denied",
            [&]
            {
                const auto folder = qEnvironmentVariable("TATSU_DENIED_SAVE_DIR");
                const auto existing = folder + "/existing.pdf";
                const auto existingHash = fileHash(existing);
                require(!existingHash.isEmpty(), "prepared NTFS fixture exists");
                Document d;
                d.open(input("D01.pdf"));
                const auto originalHash = fileHash(d.source);
                d.putSignature(0, "権限不足の確認", {70, 400}, 18, Qt::black);
                const auto before = d.pdf();
                const auto revision = d.revision;
                QJsonArray errors;
                for (const auto& path : {folder + "/new.pdf", existing})
                {
                    bool denied = false;
                    try
                    {
                        d.save(path, fileHash(path));
                    }
                    catch (const std::exception& error)
                    {
                        denied = true;
                        errors.append(QString::fromUtf8(error.what()));
                    }
                    require(denied, "NTFS denial must reject save");
                    require(d.pdf() == before && d.revision == revision && d.dirty(),
                            "access denial preserves the unsaved transaction");
                }
                require(fileHash(existing) == existingHash && fileHash(d.source) == originalHash,
                        "access denial preserves both existing destination and original");
                require(!QFileInfo::exists(folder + "/new.pdf"), "no partial new PDF");
                return QJsonObject{{"attempts", errors}, {"dirty_and_files_preserved", true}};
            });
    if (qEnvironmentVariable("TATSU_NATIVE_PDF_PRINTER") == "1")
        run("A03_Windows_PDF_printer",
            [&]
            {
                const QString name = "Microsoft Print to PDF";
                require(QPrinterInfo::availablePrinterNames().contains(name),
                        "Microsoft Print to PDF is installed");
                Document d;
                d.open(input("D01.pdf"));
                d.putSignature(0, "山田 太郎\n髙橋", {70, 400}, 22, Qt::black);
                d.save(dest("native-print-input.pdf"));
                QPrinter printer(QPrinterInfo::printerInfo(name), QPrinter::HighResolution);
                // A .pdf suffix can switch QPrinter to Qt's PDF engine. Use .prn first.
                const auto temporary = dest("windows-printer.prn");
                printer.setOutputFileName(temporary);
                printer.setOutputFormat(QPrinter::NativeFormat);
                require(printer.isValid() && printer.outputFormat() == QPrinter::NativeFormat &&
                            printer.printerName() == name,
                        "native Windows driver selected");
                printDocument(d.pdf(), printer);
                require(printer.printerState() != QPrinter::Error, "native print job succeeded");
                QElapsedTimer wait;
                wait.start();
                while (!QFileInfo::exists(temporary) && wait.elapsed() < 15000)
                    QTest::qWait(50);
                QFile generated(temporary);
                require(generated.open(QIODevice::ReadOnly) && generated.read(5) == "%PDF-",
                        "Windows driver produced PDF bytes");
                generated.close();
                require(QFile::rename(temporary, dest("windows-printer.pdf")), "archive output");
                auto printed = readPdf(dest("windows-printer.pdf"));
                require(printed.getCatalog()->getPageCount() == 1, "printed page count");
                renderPage(printed, 0, 1.2).save(dest("windows-printer.png"));
                return QJsonObject{{"printer", name},
                                   {"output_format", "NativeFormat"},
                                   {"physical_printer", "未実行"}};
            });
    run("A11_UI_open_error_routing",
        [&]
        {
            Window w;
            w.openFile(input("D01.pdf"));
            w.doc.putSignature(0, "保持する署名", {80, 400}, 20, Qt::black);
            w.refresh();
            const auto before = w.doc.pdf();
            const auto revision = w.doc.revision;
            const auto source = w.doc.source;
            int prompts = 0;
            bool providePassword = false;
            QTimer answer;
            QObject::connect(&answer, &QTimer::timeout,
                             [&]
                             {
                                 for (auto dialog : w.findChildren<QInputDialog*>())
                                     if (dialog->isVisible())
                                     {
                                         ++prompts;
                                         if (providePassword)
                                         {
                                             dialog->setTextValue("correct-password");
                                             dialog->accept();
                                         }
                                         else
                                             dialog->reject();
                                     }
                             });
            answer.start(20);
            for (const auto& path : {input("D08-broken.pdf"), dest("missing-input.pdf")})
            {
                bool failed = false;
                try
                {
                    w.openFile(path);
                }
                catch (const std::exception&)
                {
                    failed = true;
                }
                require(
                    failed && prompts == 0,
                    "damaged or missing PDFs report a read error without asking for a password");
                require(w.doc.pdf() == before && w.doc.revision == revision &&
                            w.doc.source == source && w.doc.dirty(),
                        "failed open preserves the current unsaved document");
            }
            w.openFile(input("D08-encrypted.pdf"));
            require(prompts == 1 && w.doc.pdf() == before && w.doc.revision == revision &&
                        w.doc.dirty(),
                    "cancelling a real password prompt preserves the current document");
            providePassword = true;
            w.openFile(input("D08-encrypted.pdf"));
            require(prompts == 2 && !w.doc.readOnly.isEmpty() && !w.doc.dirty() &&
                        w.doc.source.endsWith("D08-encrypted.pdf"),
                    "correct password opens the encrypted PDF read-only");
            answer.stop();
            return QJsonObject{{"invalid_inputs_rejected", 2},
                               {"password_prompts", prompts},
                               {"dialog_backend", "Qt QInputDialog, offscreen"}};
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
    run("A03_annotation_print_flags",
        [&]
        {
            Document d;
            d.open(input("D01.pdf"));
            const auto plain = renderPage(d.pdf(), 0, 1);
            auto signature = d.putSignature(0, "印刷の確認", {80, 300}, 24, Qt::black);
            auto annotation = *d.pdf().getObjectByReference(signature.ref).getDictionary();
            detail::set(annotation, "F", PDFObject::createInteger(0));
            PDFDocumentBuilder builder(&d.pdf());
            builder.setObject(signature.ref, detail::dictObject(annotation));
            auto noPrint = builder.build();
            require(renderPage(noPrint, 0, 1) != plain, "view-only annotation is visible");
            require(renderPage(noPrint, 0, 1, true, true, RenderPurpose::Print) == plain,
                    "view-only annotation does not print");
            detail::set(annotation, "F", PDFObject::createInteger(4 | 32));
            builder.setObject(signature.ref, detail::dictObject(annotation));
            auto printOnly = builder.build();
            require(renderPage(printOnly, 0, 1) == plain, "NoView annotation is hidden on screen");
            require(renderPage(printOnly, 0, 1, true, true, RenderPurpose::Print) != plain,
                    "Print annotation is present in print rendering");
            return QJsonObject{{"view_and_print_flags", "PASS"}};
        });
    run("A03_print_page_selection",
        [&]
        {
            Document d;
            d.open(input("D02.pdf"));
            QPrinter rangePrinter(QPrinter::HighResolution);
            rangePrinter.setOutputFormat(QPrinter::PdfFormat);
            rangePrinter.setOutputFileName(dest("print-range.pdf"));
            rangePrinter.setFromTo(2, 3);
            rangePrinter.setPrintRange(QPrinter::PageRange);
            printDocument(d.pdf(), rangePrinter);
            auto printed = readPdf(dest("print-range.pdf"));
            require(printed.getCatalog()->getPageCount() == 2, "only requested pages printed");
            QPrinter currentPrinter(QPrinter::HighResolution);
            currentPrinter.setOutputFormat(QPrinter::PdfFormat);
            currentPrinter.setOutputFileName(dest("print-current.pdf"));
            currentPrinter.setPrintRange(QPrinter::CurrentPage);
            printDocument(d.pdf(), currentPrinter, 2);
            auto current = readPdf(dest("print-current.pdf"));
            require(current.getCatalog()->getPageCount() == 1, "only current page printed");
            const auto expected = pageSize(d.pdf().getCatalog()->getPage(2));
            const auto actual = pageSize(current.getCatalog()->getPage(0));
            require(std::abs(expected.width() - actual.width()) < 1 &&
                        std::abs(expected.height() - actual.height()) < 1,
                    "current printed page has the requested dimensions");
            return QJsonObject{{"range_pages", 2}, {"current_page", 3}};
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
