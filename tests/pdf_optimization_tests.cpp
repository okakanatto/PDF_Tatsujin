#include "pdf_optimization_tests.h"
#include "bookmark_edit.h"
#include "form_fields.h"
#include "link_edit.h"
#include "page_decoration.h"
#include "page_operations.h"
#include "pdf_objects.h"
#include "pdf_optimization_dialog.h"
#include "pdfdocumentbuilder.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& text)
{
    if (!value)
        fail(text);
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
    fail("Invalid optimization succeeded");
}
PDFDocument seedData(const PDFDocument& document, QByteArray encoded = {}, int count = 2)
{
    PDFDocumentBuilder builder(&document);
    PDFDictionary dictionary;
    if (encoded.isEmpty())
        encoded = qCompress(QByteArray(250000, 'a'), 1).mid(4);
    detail::set(dictionary, "Filter", PDFObject::createName("FlateDecode"));
    std::vector<PDFObject> refs;
    for (int i = 0; i < count; ++i)
        refs.push_back(PDFObject::createReference(
            builder.addObject(detail::streamObject(dictionary, encoded))));
    builder.addObject(detail::streamObject({}, QByteArray(2 * 1024 * 1024, 'x')));
    auto root = *builder.getObjectByReference(builder.getCatalogReference()).getDictionary();
    detail::set(root, "TatsujinOptimizationTestData", detail::arrObject(refs));
    builder.setObject(builder.getCatalogReference(), detail::dictObject(root));
    return builder.build();
}
PDFDocument rich(const QString& fixtures)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    document.putSignature(0, "最適化しても再編集", {100, 250}, 12, Qt::black);
    document.putSignature(0, "最適化しても再編集", {100, 250}, 12, Qt::black);
    auto links = editableLinks(document.pdf());
    for (int i = 0; i < 2; ++i)
        links << LinkEntry{0, -1, {20, 20, 120, 35}, "同一だが別のリンク", LinkTarget::Page, 1, {}};
    document.commit(replaceLinks(document.pdf(), links));
    document.commit(replaceBookmarks(document.pdf(), {{{}, "日本語のしおり", -1, 2, true, true}}));
    DecorationOptions option;
    option.kind = DecorationKind::Watermark;
    option.watermark = "資料";
    option.fontFamily = signatureFont();
    document.commit(putDecoration(document.pdf(), {0, 1}, option));
    QImage image(160, 100, QImage::Format_RGB32);
    image.fill(QColor("#65a3ed"));
    document.putImage(0, OverlayKind::Image, image, {100, 350}, 70);
    document.putImage(0, OverlayKind::Image, image, {250, 350}, 70);
    return seedData(document.pdf());
}
} // namespace
QJsonObject testOptimizationLifecycle(const QString& fixtures, const QString& output)
{
    const auto original = rich(fixtures);
    const auto baseline = encodePdf(original);
    writeCandidate(original, output + "/optimization-before.pdf");
    auto result = optimizePdf(original);
    check(result.smaller() && result.sharedStreams >= 1 && result.removedObjects >= 1,
          "Real sharing, unused object removal and byte reduction");
    check(encodePdf(original) == baseline, "Input snapshot unchanged");
    auto after = result.document;
    auto before = original;
    for (int i = 0; i < int(original.getCatalog()->getPageCount()); ++i)
        check(renderPage(before, i, .6) == renderPage(after, i, .6) &&
                  pageText(before, i) == pageText(after, i),
              "Visible pixels and body text exact");
    auto links = editableLinks(after);
    const auto pageObject =
        after.getObjectByReference(after.getCatalog()->getPage(0)->getPageReference());
    const auto annots = after.getObject(pageObject.getDictionary()->get("Annots"));
    check(links.size() == 2 && annots.getArray()->getItem(links[0].sourceIndex) !=
                                   annots.getArray()->getItem(links[1].sourceIndex),
          "Identical link dictionaries retain separate identities");
    const auto items = signatures(after, 0);
    check(items.size() == 4 && items[0].ref != items[1].ref,
          "Signatures and images remain independent editing targets");
    check(decorationGroups(after).size() == 1 && editableBookmarks(after).size() == 1,
          "Decoration settings and bookmark retain structure");
    Document document;
    document.history = {original};
    document.saved = -1;
    document.commit(after);
    document.undo();
    check(document.pdf() == original, "One Undo restores original optimization state");
    document.redo();
    document.save(output + "/optimization-after.pdf");
    Document reopened;
    reopened.open(document.target);
    auto values = editableLinks(reopened.pdf());
    values[0].description = "片方だけ再編集";
    reopened.commit(replaceLinks(reopened.pdf(), values));
    auto signaturesBefore = signatures(reopened.pdf(), 0);
    reopened.moveSignature(0, signaturesBefore[0], {10, 20});
    check(signatures(reopened.pdf(), 0)[1].rect == signaturesBefore[1].rect,
          "Moving one signature does not move the other shared appearance");
    reopened.save(output + "/optimization-reedited.pdf");
    check(editableLinks(reopened.pdf())[1].description == "同一だが別のリンク",
          "One link edit leaves identical neighbor unchanged");
    const auto again = optimizePdf(after);
    check(!again.smaller() && again.document == after,
          "Already optimized state yields no applyable change");
    Document form;
    form.open(fixtures + "/D07.pdf");
    for (const auto& field : formFields(form.pdf()))
        if (field.name == "name")
            putFormValue(form, field.widget, {"髙橋 香織"});
    form.commit(seedData(form.pdf()));
    writeCandidate(form.pdf(), output + "/optimization-form-before.pdf");
    form.commit(optimizePdf(form.pdf()).document);
    form.save(output + "/optimization-form-after.pdf");
    Document scan;
    scan.open(fixtures + "/D06.pdf");
    scan.commit(seedData(scan.pdf()));
    writeCandidate(scan.pdf(), output + "/optimization-mixed-before.pdf");
    scan.commit(optimizePdf(scan.pdf()).document);
    scan.save(output + "/optimization-mixed-after.pdf");
    return {{"before_bytes", result.beforeBytes},
            {"after_bytes", result.afterBytes},
            {"shared_streams", result.sharedStreams},
            {"removed_objects", result.removedObjects},
            {"pixel_difference", 0},
            {"independent_editing_identity", true},
            {"one_Undo_save_reedit", true}};
}
QJsonObject testOptimizationFailures(const QString& fixtures)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    auto input = seedData(source);
    const auto baseline = encodePdf(input);
    QJsonArray rows;
    auto invalid = [&](QString name, const std::function<void()>& operation)
    {
        rows << QJsonObject{{"case", name}, {"error", rejects(operation)}};
        check(encodePdf(input) == baseline, "Failure leaves original unchanged");
    };
    invalid("empty document", [&] { optimizePdf(PDFDocument{}); });
    invalid("signed document", [&] { optimizePdf(readPdf(fixtures + "/D08-signed.pdf")); });
    invalid("invalid flate", [&] { optimizePdf(seedData(source, "invalid-flate")); });
    invalid("input over 64MiB",
            [&] { optimizePdf(seedData(source, QByteArray(64 * 1024 * 1024 + 1, 'a'), 1)); });
    const auto tooLarge = qCompress(QByteArray(129 * 1024 * 1024, 'a'), 1).mid(4);
    invalid("decoded over 128MiB", [&] { optimizePdf(seedData(source, tooLarge, 1)); });
    const auto repeated = qCompress(QByteArray(105 * 1024 * 1024, 'a'), 1).mid(4);
    invalid("decoded total over 512MiB", [&] { optimizePdf(seedData(source, repeated, 5)); });
    bool cancelled = false;
    invalid("cancel after stream processing",
            [&]
            {
                optimizePdf(
                    input, [&] { return cancelled; },
                    [&](QString text)
                    {
                        if (text.startsWith("参照"))
                            cancelled = true;
                    });
            });
    check(cancelled, "Cancellation after real stream processing");
    // A multi-filter stream's raw bytes are not zlib input. Preserve it exactly.
    PDFDocumentBuilder builder(&source);
    PDFDictionary dictionary;
    detail::set(dictionary, "Filter",
                detail::arrObject({PDFObject::createName("ASCIIHexDecode"),
                                   PDFObject::createName("FlateDecode")}));
    auto data = qCompress(QByteArray("multiple filter payload"), 1).mid(4).toHex() + ">";
    auto reference = builder.addObject(detail::streamObject(dictionary, data));
    auto catalog = *builder.getObjectByReference(builder.getCatalogReference()).getDictionary();
    detail::set(catalog, "TatsujinOptimizationTestData", PDFObject::createReference(reference));
    builder.setObject(builder.getCatalogReference(), detail::dictObject(catalog));
    auto multiple = builder.build();
    auto output = optimizePdf(multiple).document;
    const auto root = output.getObject(output.getTrailerDictionary()->get("Root"));
    const auto stream = output.getObject(root.getDictionary()->get("TatsujinOptimizationTestData"));
    check(*stream.getStream()->getContent() == data,
          "Multiple filters are not recompressed as bare Flate");
    return {{"rejected", rows}, {"multi_filter_bytes_exact", true}, {"input_unchanged", true}};
}
QJsonObject testOptimizationUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.history = {rich(fixtures)};
    window.doc.saved = -1;
    window.refresh(true);
    window.show();
    const auto original = window.doc.pdf();
    int ticks = 0, mode = 0;
    QString error;
    auto operate = [&]
    {
        QElapsedTimer deadline;
        deadline.start();
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        bool handled = false;
        QObject::connect(
            &timer, &QTimer::timeout,
            [&]
            {
                ++ticks;
                auto dialog =
                    dynamic_cast<PdfOptimizationDialog*>(QApplication::activeModalWidget());
                if (!dialog || handled)
                    return;
                if (deadline.elapsed() > 25000)
                {
                    error = "Optimization UI deadline";
                    dialog->reject();
                    handled = true;
                    return;
                }
                auto apply = dialog->findChild<QPushButton*>("optimizationApply");
                if (mode == 1)
                {
                    bool working = false;
                    for (auto job : dialog->findChildren<QThread*>())
                        working |= job->isRunning();
                    if (working)
                    {
                        dialog->reject();
                        handled = true;
                    }
                    else if (apply->isEnabled())
                    {
                        error = "Cancel did not observe actual worker";
                        dialog->reject();
                        handled = true;
                    }
                }
                else if (mode == 2 && !dialog->findChild<QProgressBar*>()->isVisible())
                {
                    if (apply->isEnabled() || dialog->property("optimizationSmaller").toBool())
                        error = "No reduction cannot apply";
                    dialog->reject();
                    handled = true;
                }
                else if (mode == 0 && apply->isEnabled())
                {
                    dialog->resize(520, 300);
                    dialog->grab().save(output + "/optimization-ui.png");
                    QTest::mouseClick(apply, Qt::LeftButton);
                    handled = true;
                }
            });
        timer.start(2);
        window.optimizeDocument();
        timer.stop();
        check(error.isEmpty(), error);
    };
    operate();
    check(window.doc.cursor == 1, "Actual comparison and explicit apply make one commit");
    const auto optimized = window.doc.pdf();
    window.doc.save(output + "/optimization-ui.pdf");
    window.undoAction->trigger();
    check(window.doc.pdf() == original, "Product Undo restores original");
    mode = 1;
    operate();
    check(window.doc.pdf() == original && window.doc.cursor == 0,
          "Worker cancellation keeps document and Undo state");
    window.redoAction->trigger();
    check(window.doc.pdf() == optimized, "Redo restores optimized state");
    mode = 2;
    operate();
    check(window.doc.pdf() == optimized && window.doc.cursor == 1,
          "No-reduction dialog makes no commit");
    return {{"actual_compare_apply_Undo_Redo_cancel_no_reduction", true},
            {"GUI_event_ticks", ticks},
            {"native_UI", "未実行"}};
}
QJsonObject testOptimizationOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.history = {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6})};
    window.doc.saved = -1;
    window.doc.putSignature(0, "OCRと最適化を保持", {30, 30}, 10, Qt::black);
    window.refresh(true);
    window.show();
    QString error;
    QTimer messages;
    QObject::connect(&messages, &QTimer::timeout,
                     [&]
                     {
                         if (auto box =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (box->windowTitle() != "OCR結果")
                                 error = box->text();
                             box->accept();
                         }
                     });
    messages.start(10);
    window.language->setCurrentIndex(0);
    window.scope->setCurrentIndex(0);
    window.startOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 90000),
          "Real bilingual OCR before optimization");
    check(error.isEmpty(), error);
    window.doc.commit(seedData(window.doc.pdf()));
    auto before = window.doc.pdf();
    writeCandidate(before, output + "/optimization-ocr-before.pdf");
    auto result = optimizePdf(before);
    check(result.smaller(), "OCR PDF optimization reduces actual bytes");
    window.doc.commit(result.document);
    for (int i = 0; i < 2; ++i)
        check(pageText(before, i) == pageText(window.doc.pdf(), i) &&
                  renderPage(before, i, .8) == renderPage(window.doc.pdf(), i, .8),
              "OCR copy/search and visible pixels exact");
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "Fixed JP/EN terms survive optimization");
    window.refresh(true);
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    window.query->setText("市民公園");
    QTest::keyClick(window.query, Qt::Key_Return);
    auto search = window.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              15000),
          "Actual product search after optimization");
    window.canvas->setZoom(.5);
    window.canvas->goToPage(0);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "Optimized OCR page ready for selection");
    window.canvas->setFocus();
    const auto physical = pageSize(window.doc.pdf().getCatalog()->getPage(0));
    const auto first = window.canvas->mapFromScene({1, 1});
    const auto last = window.canvas->mapFromScene({physical.width() - 1, physical.height() - 1});
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 60);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "Optimized OCR selection ready");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto copied = QApplication::clipboard()->text();
    check(copied.size() > 500 && copied.contains("市民公園"), "Actual Qt copy after optimization");
    window.doc.save(output + "/optimization-ocr-after.pdf");
    auto reopened = readPdf(window.doc.target);
    check(signatures(reopened, 0).size() == 1 && pageText(reopened, 0).contains("市民公園"),
          "Saved reeditable signature and OCR retained");
    return {{"real_bilingual_OCR", true},
            {"Qt_copy_characters", copied.size()},
            {"pixel_difference", 0},
            {"body_text_exact", true},
            {"before_bytes", result.beforeBytes},
            {"after_bytes", result.afterBytes}};
}
} // namespace tatsu
