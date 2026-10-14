#include "redaction_scan_tests.h"
#include "ocr_preprocess.h"
#include "page_operations.h"
#include "redaction_ui_test_support.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
QString selectAndCopy(Window& window, int page)
{
    window.canvas->setZoom(.5);
    window.canvas->goToPage(page);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(page); }, 15000),
          "Saved OCR page ready");
    const auto crop = window.doc.pdf().getCatalog()->getPage(page)->getCropBox();
    const auto first =
        window.canvas->pdfToViewport(page, crop.topLeft() + QPointF(1, crop.height() - 1))
            .toPoint();
    const auto last =
        window.canvas->pdfToViewport(page, crop.topLeft() + QPointF(crop.width() - 1, 1)).toPoint();
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 60);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "Saved OCR selection ready");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    return QApplication::clipboard()->text();
}
void search(Window& window, const QString& term)
{
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    window.query->setText(term);
    QTest::keyClick(window.query, Qt::Key_Return);
    auto panel = window.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor([&] { return panel->session()->complete(); }, 15000) &&
              !panel->session()->matches().isEmpty(),
          "Saved OCR term found through actual search");
}
} // namespace
QJsonObject testRedactionScanOcr(const QString& fixtures, const QString& output)
{
    QFile file(fixtures + "/redaction-copy/scan-then-ocr.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen scan redaction criteria");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    check(parse.error == QJsonParseError::NoError && document.isObject(), "Valid frozen criteria");
    const auto criteria = document.object();
    const auto sourcePath = fixtures + "/" + criteria["source"].toString();
    check(fileHash(sourcePath).toHex() == criteria["source_sha256"].toString().toLatin1(),
          "Frozen scan input hash");
    Document original;
    original.history = {selectPages(readPdf(sourcePath), {2, 6})};
    original.saved = -1;
    original.putSignature(1, criteria["signature"].toString(), {30, 30}, 10, Qt::black);
    const auto before = encodePdf(original.pdf());
    const auto cursor = original.cursor;
    const auto rectangle = criteria["raw_pdf_rectangle"].toArray();
    const QRectF raw(rectangle[0].toDouble(), rectangle[1].toDouble(), rectangle[2].toDouble(),
                     rectangle[3].toDouble());
    const auto imageCopy = output + "/redaction-scan-before-ocr.pdf";
    writeCandidate(original.pdf(), output + "/redaction-scan-source.pdf");
    RedactionDialog dialog(original.pdf(), 0, {}, imageCopy);
    dialog.show();
    const auto physical = pageMatrix(original.pdf().getCatalog()->getPage(0)).mapRect(raw);
    testing::add(dialog, physical);
    dialog.findChild<QCheckBox*>("redactionConsent")->setChecked(true);
    check(dialog.grab().save(output + "/redaction-scan-range.png"), "Actual scan range screenshot");
    dialog.findChild<QPushButton*>("saveRedactedCopy")->click();
    check(QTest::qWaitFor(
              [&] { return !dialog.findChild<QProgressBar*>("redactionProgress")->isVisible(); },
              30000) &&
              !dialog.savedPath().isEmpty(),
          dialog.findChild<QLabel*>("redactionMessage")->text());
    Window copy;
    copy.doc.open(imageCopy);
    copy.refresh(true);
    copy.show();
    check(pageText(copy.doc.pdf(), 0).trimmed().isEmpty(), "Image copy has no old OCR layer");
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
    copy.language->setCurrentIndex(0);
    copy.scope->setCurrentIndex(0);
    copy.startOcr();
    check(QTest::qWaitFor([&] { return !copy.doc.busy && !copy.worker; }, 90000),
          "Actual bilingual OCR of redacted scan completes");
    messages.stop();
    check(error.isEmpty(), error);
    writeCandidate(copy.doc.pdf(), output + "/redaction-scan-ocr-candidate.pdf");
    const auto japanese = pageText(copy.doc.pdf(), 0);
    const auto english = pageText(copy.doc.pdf(), 1);
    QFile observed(output + "/redaction-scan-ocr-observed.json");
    check(observed.open(QIODevice::WriteOnly), "Write OCR observations before fixed checks");
    observed.write(
        QJsonDocument(QJsonObject{{"japanese", japanese}, {"english", english}}).toJson());
    observed.close();
    check(!japanese.contains(criteria["remove"].toString()), "Masked target absent after OCR");
    for (const auto& term : criteria["keep_japanese"].toArray())
        check(japanese.contains(term.toString()), "Frozen outside Japanese term preserved");
    for (const auto& term : criteria["keep_english"].toArray())
        check(english.contains(term.toString(), Qt::CaseInsensitive),
              "Frozen outside English term preserved");
    const auto recognized = encodePdf(copy.doc.pdf());
    copy.doc.undo();
    check(pageText(copy.doc.pdf(), 0).trimmed().isEmpty(), "OCR Undo restores redacted image copy");
    copy.doc.redo();
    check(encodePdf(copy.doc.pdf()) == recognized, "OCR Redo restores recognized copy");
    const auto saved = output + "/redaction-scan-ocr-saved.pdf";
    copy.doc.save(saved);
    Window reopened;
    reopened.doc.open(saved);
    reopened.refresh(true);
    reopened.show();
    search(reopened, "管理事務所");
    const auto copiedJapanese = selectAndCopy(reopened, 0);
    check(!copiedJapanese.contains(criteria["remove"].toString()) &&
              copiedJapanese.contains("管理事務所") && copiedJapanese.contains("花壇"),
          "Saved Japanese Qt selection/copy excludes target");
    search(reopened, "coastal");
    const auto copiedEnglish = selectAndCopy(reopened, 1);
    check(copiedEnglish.contains("coastal", Qt::CaseInsensitive) &&
              copiedEnglish.contains("surveys", Qt::CaseInsensitive),
          "Saved English Qt selection/copy preserved");
    check(signatures(reopened.doc.pdf(), 1).size() == 1 &&
              signatures(reopened.doc.pdf(), 1).first().text == criteria["signature"].toString(),
          "Japanese signature survives redaction, OCR and reopen");
    check(encodePdf(original.pdf()) == before && original.cursor == cursor &&
              original.saved == -1 && original.dirty(),
          "Original unsaved snapshot/history retained");
    original.undo();
    check(signatures(original.pdf(), 1).isEmpty(), "Original signature Undo retained");
    original.redo();
    check(encodePdf(original.pdf()) == before &&
              fileHash(sourcePath).toHex() == criteria["source_sha256"].toString().toLatin1(),
          "Original Redo and input bytes retained");
    return {{"partial_scan_redaction_before_actual_bilingual_OCR", true},
            {"Japanese_Qt_copied_characters", copiedJapanese.size()},
            {"English_Qt_copied_characters", copiedEnglish.size()},
            {"signature_and_original_history_preserved", true},
            {"native_clipboard", "未実行"}};
}
QJsonObject testRedactionOcrMasks()
{
    const QRectF box(10, 10, 60, 25);
    QPainterPath rectangle;
    rectangle.addRect(box);
    check(axisAlignedRectangle(rectangle) == box, "Single rectangular path accepted");
    QTransform rotated;
    rotated.rotate(25);
    check(!axisAlignedRectangle(rotated.map(rectangle)), "Oblique filled path not omitted");
    QImage source(96, 96, QImage::Format_Grayscale8);
    source.fill(Qt::white);
    {
        QPainter painter(&source);
        painter.fillRect(box, Qt::black);
    }
    const auto before = source;
    auto input = source;
    check(omitSolidBlackOcrBlocks(input, {box}) == 1 && input != before && source == before,
          "Opaque block omitted only from independent recognition raster");
    auto whiteText = source;
    {
        QPainter painter(&whiteText);
        painter.setPen(Qt::white);
        painter.drawText(box, Qt::AlignCenter, "A");
    }
    const auto legible = whiteText;
    check(omitSolidBlackOcrBlocks(whiteText, {box}) == 0 && whiteText == legible,
          "White glyph inside black rectangle preserved for OCR");
    auto translucent = source;
    translucent.fill(Qt::white);
    {
        QPainter painter(&translucent);
        painter.fillRect(box, QColor(0, 0, 0, 128));
    }
    const auto gray = translucent;
    check(omitSolidBlackOcrBlocks(translucent, {box}) == 0 && translucent == gray,
          "Nonblack visible pixels are not omitted");
    auto invalid = source;
    check(omitSolidBlackOcrBlocks(invalid, {QRectF(-5, -5, 200, 200), QRectF(10, 10, 2, 2)}) == 0 &&
              invalid == source,
          "Clipped or tiny rectangles are not omitted");
    return {{"visible_white_glyph_and_translucent_pixels_preserved", true},
            {"source_raster_unchanged", true},
            {"oblique_clipped_tiny_paths_preserved", true}};
}
} // namespace tatsu
