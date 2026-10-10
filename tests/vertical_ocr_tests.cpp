#include "vertical_ocr_tests.h"
#include "ocr_language.h"
#include "pdf_font_metrics.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "vertical_text_spacing.h"
#include "window.h"
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& why)
{
    if (!value)
        fail(why);
}
QJsonObject criteria(const QString& fixtures)
{
    QFile file(fixtures + "/vertical-ocr-criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen vertical criterion");
    const auto bytes = file.readAll();
    check(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() ==
              "516522131307b548f3b46e8d8a274b398d732857455806c8df2484ee40f3e604",
          "Frozen vertical criterion SHA");
    const auto fixed = QJsonDocument::fromJson(bytes).object();
    check(fileHash(fixtures + "/" + fixed["source"].toString()).toHex() ==
              fixed["source_sha256"].toString().toLatin1(),
          "Frozen vertical PDF SHA");
    return fixed;
}
QString normalized(QString text)
{
    return text.remove(QRegularExpression("\\s"));
}
QString expected(const QJsonObject& fixed)
{
    QString result;
    for (const auto& line : fixed["expected_lines_right_to_left"].toArray())
        result += line.toString();
    return result;
}
void selectVertical(Window& window)
{
    window.ocrAction->trigger();
    window.language->setFocus();
    QTest::keyClick(window.language, Qt::Key_End);
    check(window.language->currentIndex() == 3 &&
              window.language->currentText() == "日本語（縦書き）" &&
              window.findChild<QLabel*>("ocrAccuracy")->text().contains("右から左"),
          "Actual vertical choice and scope guidance");
}
void checkSearch(Window& window, const QJsonObject& fixed)
{
    for (const auto& value : fixed["search_terms"].toArray())
    {
        const auto row = value.toObject();
        window.canvas->setFocus();
        QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
        window.query->setText(row["term"].toString());
        QTest::keyClick(window.query, Qt::Key_Return);
        auto panel = window.findChild<SearchPanel*>("searchPanel");
        check(QTest::qWaitFor([&] { return panel->session()->complete(); }, 15000) &&
                  panel->session()->matches().size() == 1,
              "Actual vertical search: " + row["term"].toString());
        const auto match = panel->session()->matches().first();
        check(match.page == 2, "Search returns frozen vertical page");
        const auto actual =
            pageMatrix(window.doc.pdf().getCatalog()->getPage(2), 1, false).mapRect(match.bounds);
        const auto box = row["qt_bounds_pt"].toArray();
        const double difference =
            qMax(qMax(qAbs(actual.x() - box[0].toDouble()), qAbs(actual.y() - box[1].toDouble())),
                 qMax(qAbs(actual.width() - box[2].toDouble()),
                      qAbs(actual.height() - box[3].toDouble())));
        check(difference * 25.4 / 72 <= fixed["maximum_bounds_error_mm"].toDouble(),
              "Frozen 2mm vertical search geometry");
    }
}
QString selectAndCopy(Window& window)
{
    window.canvas->setZoom(.5);
    window.canvas->goToPage(2);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(2); }, 15000),
          "Vertical page ready for actual selection");
    const auto crop = window.doc.pdf().getCatalog()->getPage(2)->getCropBox();
    const auto first =
        window.canvas->pdfToViewport(2, {crop.right() - 1, crop.bottom() - 1}).toPoint();
    const auto last = window.canvas->pdfToViewport(2, {crop.left() + 1, crop.top() + 1}).toPoint();
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 60);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "Actual vertical text selection ready");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    return QApplication::clipboard()->text();
}
} // namespace
QJsonObject testVerticalOcrUi(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    const auto source = fixtures + "/" + fixed["source"].toString();
    Window window;
    window.doc.open(source);
    window.doc.putSignature(2, "縦書き署名", {80, 60}, 12, Qt::black);
    window.refresh(true);
    window.show();
    window.canvas->goToPage(2);
    selectVertical(window);
    window.scope->setCurrentIndex(1);
    const auto before = encodePdf(window.doc.pdf());
    const auto cursor = window.doc.cursor;
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout,
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
    dismiss.start(10);
    window.startOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 90000),
          "Actual vertical worker completes");
    dismiss.stop();
    check(error.isEmpty(), error);
    const auto text = pageText(window.doc.pdf(), 2);
    writeCandidate(window.doc.pdf(), output + "/vertical-ui-candidate.pdf");
    QJsonArray lines;
    const auto layout = textLayout(window.doc.pdf(), 2);
    for (const auto& block : layout.getTextBlocks())
        for (const auto& line : block.getLines())
        {
            QString content;
            for (const auto& character : line.getCharacters())
                content += character.character;
            lines.append(QJsonObject{
                {"text", content},
                {"angle", line.getCharacters().empty() ? 0 : line.getCharacters().front().angle}});
        }
    QFile observed(output + "/vertical-observed.json");
    check(observed.open(QIODevice::WriteOnly), "Preserve vertical observations before assertions");
    observed.write(QJsonDocument(QJsonObject{{"text", text}, {"lines", lines}}).toJson());
    observed.close();
    check(normalized(text) == expected(fixed), "Frozen exact vertical content and column order");
    check(window.doc.cursor == cursor + 1 && signatures(window.doc.pdf(), 2).size() == 1,
          "One OCR commit retains existing signature");
    for (int page = 0; page < window.doc.pages(); ++page)
    {
        auto original = readPdf(source);
        check(renderPage(original, page, .5, false) ==
                  renderPage(window.doc.pdf(), page, .5, false),
              "All original visible pages unchanged");
    }
    checkSearch(window, fixed);
    const auto copied = selectAndCopy(window);
    QFile clipboard(output + "/vertical-ui-copied.txt");
    check(clipboard.open(QIODevice::WriteOnly), "Preserve actual Qt copied text");
    clipboard.write(copied.toUtf8());
    clipboard.close();
    check(normalized(copied) == expected(fixed),
          "Actual Qt copy follows top-to-bottom, right-to-left");
    check(window.grab().save(output + "/vertical-ocr-ui.png"), "Actual vertical UI screenshot");
    const auto recognized = encodePdf(window.doc.pdf());
    window.doc.undo();
    check(encodePdf(window.doc.pdf()) == before && window.doc.cursor == cursor,
          "OCR Undo preserves unsaved signature and original image");
    window.doc.redo();
    check(encodePdf(window.doc.pdf()) == recognized, "OCR Redo restores complete layer");
    const auto saved = output + "/vertical-ui-saved.pdf";
    window.doc.save(saved);
    Window reopened;
    reopened.doc.open(saved);
    reopened.refresh(true);
    reopened.show();
    checkSearch(reopened, fixed);
    check(normalized(selectAndCopy(reopened)) == expected(fixed), "Saved vertical selection/copy");
    const auto old = signatures(reopened.doc.pdf(), 2).first();
    reopened.doc.putSignature(2, "再編集署名", old.rect.topLeft(), old.size, old.color, old.ref,
                              old.fontFamily);
    reopened.doc.save(output + "/vertical-ui-reedited.pdf");
    check(signatures(reopened.doc.pdf(), 2).first().text == "再編集署名" &&
              fileHash(source).toHex() == fixed["source_sha256"].toString().toLatin1(),
          "Signature reedit and frozen source bytes retained");
    return {{"actual_vertical_choice_OCR_search_selection_copy", true},
            {"signature_Undo_Redo_save_reopen_reedit", true},
            {"all_visible_pages_unchanged", true},
            {"native_IME_OS_clipboard_external_GUI", "未実行"}};
}
QJsonObject testVerticalOcrFailure(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    Window window;
    window.doc.open(fixtures + "/" + fixed["source"].toString());
    window.doc.putSignature(2, "取消前署名", {80, 60}, 12, Qt::black);
    window.refresh(true);
    window.show();
    selectVertical(window);
    window.scope->setCurrentIndex(2);
    window.range->setText("3,4");
    const auto before = encodePdf(window.doc.pdf());
    const auto cursor = window.doc.cursor;
    window.startOcr();
    check(
        QTest::qWaitFor(
            [&] { return window.worker && window.progress->text().startsWith("OCR 1 /"); }, 90000),
        "Vertical cancellation reaches actual partial progress");
    const auto job = window.work->path();
    window.stopOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 15000) &&
              encodePdf(window.doc.pdf()) == before && window.doc.cursor == cursor &&
              !QFileInfo::exists(job),
          "Vertical cancellation discards partial layer and owned files");
    bool refused = false;
    try
    {
        OcrJob::prepare(window.doc, {"jpn+jpn_vert", "3"});
    }
    catch (const std::exception&)
    {
        refused = true;
    }
    check(refused, "Mixed model request refused; horizontal defaults unchanged");
    auto assets = privateTemporaryDirectory(QDir::tempPath() + "/tatsujin-missing-vertical-XXXXXX");
    check(assets->isValid() && QDir().mkpath(assets->filePath("fonts")) &&
              QFile::copy(asset("fonts/NotoSansJP.ttf"), assets->filePath("fonts/NotoSansJP.ttf")),
          "Owned missing-model test assets");
    auto failedJob = OcrJob::prepare(window.doc, {"jpn_vert", "3"});
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("TATSU_ASSETS", assets->path());
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), failedJob->workerArguments());
    check(child.waitForStarted() && child.waitForFinished(30000) && child.exitCode() == 2 &&
              !QFileInfo::exists(failedJob->filePath("result.pdf")) &&
              encodePdf(window.doc.pdf()) == before && window.doc.cursor == cursor,
          "Missing vertical model publishes no candidate and retains document");
    QFile error(output + "/vertical-missing-model-stderr.txt");
    check(error.open(QIODevice::WriteOnly), "Preserve missing-model diagnostic");
    error.write(child.readAllStandardError());
    int geometryRefusals = 0;
    for (const QString mode : {QString("rotation"), QString("unit")})
    {
        Document unsupported;
        unsupported.open(fixtures + "/" + fixed["source"].toString());
        unsupported.putSignature(2, "保持する署名", {80, 60}, 12, Qt::black);
        if (mode == "rotation")
            unsupported.rotate(2);
        else
        {
            PDFDocumentBuilder builder(&unsupported.pdf());
            const auto reference = unsupported.pdf().getCatalog()->getPage(2)->getPageReference();
            auto dictionary = *unsupported.pdf().getObjectByReference(reference).getDictionary();
            detail::set(dictionary, "UserUnit", detail::number(1.5));
            builder.setObject(reference, detail::dictObject(dictionary));
            unsupported.commit(builder.build());
        }
        const auto retained = encodePdf(unsupported.pdf());
        const auto retainedCursor = unsupported.cursor;
        bool refusedGeometry = false;
        try
        {
            OcrJob::prepare(unsupported, {"jpn_vert", "1,3"});
        }
        catch (const std::exception& exception)
        {
            refusedGeometry = QString::fromUtf8(exception.what()).contains("3ページ");
        }
        check(refusedGeometry && encodePdf(unsupported.pdf()) == retained &&
                  unsupported.cursor == retainedCursor,
              "Unsupported geometry refuses whole job and keeps unsaved edits: " + mode);
        auto horizontal = OcrJob::prepare(unsupported, {"jpn+eng", "3"});
        check(bool(horizontal), "Existing horizontal geometry support unchanged");
        const auto input = output + "/vertical-refused-" + mode + ".pdf";
        const auto settings = output + "/vertical-refused-" + mode + ".json";
        const auto candidate = output + "/vertical-refused-" + mode + "-candidate.pdf";
        writeCandidate(unsupported.pdf(), input);
        QFile configuration(settings);
        check(configuration.open(QIODevice::WriteOnly), "Write owned refusal worker settings");
        configuration.write("{\"language\":\"jpn_vert\",\"pages\":\"1,3\"}");
        configuration.close();
        QProcess worker;
        worker.start(QCoreApplication::applicationFilePath(),
                     {"--ocr-worker", input, candidate, settings,
                      output + "/vertical-refused-" + mode + "-report.json"});
        check(worker.waitForStarted() && worker.waitForFinished(30000) && worker.exitCode() == 2 &&
                  !QFileInfo::exists(candidate) &&
                  fileHash(input) == QCryptographicHash::hash(retained, QCryptographicHash::Sha256),
              "Independent worker refuses before producing partial PDF: " + mode);
        ++geometryRefusals;
    }
    return {{"actual_partial_progress_cancel", true},
            {"missing_model_refused", true},
            {"invalid_model_combination_refused", true},
            {"original_unsaved_state_retained", true},
            {"unsupported_geometry_refusals", geometryRefusals},
            {"geometry_success_condition", "未達（追加試験FAILを保持）"}};
}
QJsonObject testVerticalFontMetrics(const QString& fixtures)
{
    using namespace detail;
    const auto document = readPdf(fixtures + "/D01.pdf");
    const auto makeFont = [&](bool vertical, PDFObject widths)
    {
        PDFDictionary info;
        set(info, "Registry", PDFObject::createString("Adobe"));
        set(info, "Ordering", PDFObject::createString("Identity"));
        set(info, "Supplement", PDFObject::createInteger(0));
        PDFDictionary descendant;
        set(descendant, "Type", PDFObject::createName("Font"));
        set(descendant, "Subtype", PDFObject::createName("CIDFontType2"));
        set(descendant, "BaseFont", PDFObject::createName("NotoSansJP-Regular"));
        set(descendant, "CIDSystemInfo", dictObject(info));
        set(descendant, "DW", number(1000));
        set(descendant, "W", arrObject({PDFObject::createInteger(1), arrObject({number(700)})}));
        set(descendant, "DW2", arrObject({number(0), number(-1700)}));
        set(descendant, "W2", std::move(widths));
        PDFDictionary font;
        set(font, "Type", PDFObject::createName("Font"));
        set(font, "Subtype", PDFObject::createName("Type0"));
        set(font, "BaseFont", PDFObject::createName("NotoSansJP-Regular"));
        set(font, "Encoding", PDFObject::createName(vertical ? "Identity-V" : "Identity-H"));
        set(font, "DescendantFonts", arrObject({dictObject(descendant)}));
        return PDFFont::createFont(dictObject(font), "metric-test", &document);
    };
    const auto individual =
        arrObject({PDFObject::createInteger(1), arrObject({number(-1400), number(0), number(0)})});
    const auto vertical = makeFont(true, individual);
    check(pdfGlyphAdvance(vertical, 1, {}, {}) == -1400 &&
              pdfGlyphAdvance(vertical, 4, {}, {}) == -1700,
          "Declared individual W2 and DW2 override horizontal widths");
    const auto range =
        makeFont(true, arrObject({PDFObject::createInteger(2), PDFObject::createInteger(4),
                                  number(-2200), number(0), number(0)}));
    check(pdfGlyphAdvance(range, 2, {}, {}) == -2200 &&
              pdfGlyphAdvance(range, 4, {}, {}) == -2200 &&
              pdfGlyphAdvance(range, 5, {}, {}) == -1700,
          "Declared W2 CID range boundaries");
    const auto horizontal = makeFont(false, individual);
    check(pdfGlyphAdvance(horizontal, 1, {}, {}) == 700 &&
              pdfGlyphAdvance(horizontal, 4, {}, {}) == 1000,
          "Horizontal W/DW remain unchanged");
    int refused = 0;
    for (const auto& invalid :
         {arrObject(
              {PDFObject::createInteger(65536), arrObject({number(-1000), number(0), number(0)})}),
          arrObject({PDFObject::createInteger(1), arrObject({number(-1000), number(0)})}),
          arrObject({PDFObject::createInteger(4), PDFObject::createInteger(2), number(-1000),
                     number(0), number(0)})})
        try
        {
            makeFont(true, invalid);
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    check(refused == 3, "Invalid W2 CID/triple/range refuse safely");
    check(keepVerticalJapaneseSpacing(90, QChar(u'縦'), QChar(u'書')) &&
              keepVerticalJapaneseSpacing(270, QChar(u'す'), QChar(u'。')) &&
              !keepVerticalJapaneseSpacing(0, QChar(u'縦'), QChar(u'書')) &&
              !keepVerticalJapaneseSpacing(90, QChar(u'A'), QChar(u'B')) &&
              !keepVerticalJapaneseSpacing(90, QChar(u'字'), QChar(u' ')),
          "Japanese vertical tracking; horizontal, Latin and encoded spaces retained");
    return {{"individual_range_default_vertical_widths", true},
            {"horizontal_widths_unchanged", true},
            {"invalid_width_refusals", refused}};
}
} // namespace tatsu
