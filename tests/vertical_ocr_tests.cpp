#include "vertical_ocr_tests.h"
#include "body_text_font.h"
#include "ocr_language.h"
#include "pdf_font_metrics.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "vertical_text_spacing.h"
#include "window.h"
#include "worker_channels.h"
#include <QtTest/QTest>
#include <array>

namespace tatsu
{
namespace
{
void check(bool value, const QString& why)
{
    if (!value)
        fail(why);
}
struct FrozenProfile
{
    const char* directory;
    const char* criterion;
    const char* digest;
};
const FrozenProfile& frozenProfile(VerticalProfile profile)
{
    static const std::array<FrozenProfile, 5> profiles{{
        {"/", "vertical-ocr-criteria.json",
         "516522131307b548f3b46e8d8a274b398d732857455806c8df2484ee40f3e604"},
        {"/vertical-ocr-unit/", "criteria.json",
         "86afa075759f530758ed6e4580622c9743a2cd9c3bf3c9fdc86791d8ca7e8beb"},
        {"/vertical-ocr-geometry/", "criteria.json",
         "4e087d6138ce25f08c7f5cc70b0b2ee2b85a7c6da69bab370d2eb74ed9417ac8"},
        {"/vertical-ocr-rotations/180/", "criteria.json",
         "66eb2f2f58a96f1bd52d469bc08d7421998bc1ea4fd5d66fcfea05d3c547ca7d"},
        {"/vertical-ocr-rotations/270/", "criteria.json",
         "f532130ff146ed8e09f32f34cdb8e2950d0218d503dac506d4eefe27a3b62c7f"},
    }};
    return profiles.at(size_t(profile));
}
QJsonObject criteria(const QString& fixtures, VerticalProfile profile = VerticalProfile::Standard)
{
    const auto& paths = frozenProfile(profile);
    QFile file(fixtures + paths.directory + paths.criterion);
    check(file.open(QIODevice::ReadOnly), "Read frozen vertical criterion");
    const auto bytes = file.readAll();
    check(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == paths.digest,
          "Frozen vertical criterion SHA");
    const auto fixed = QJsonDocument::fromJson(bytes).object();
    check(fileHash(QFileInfo(file).absolutePath() + "/" + fixed["source"].toString()).toHex() ==
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
    window.canvas->goToPage(2);
    window.canvas->fitPage();
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
static QJsonObject verticalOcrUi(const QString& fixtures, const QString& output,
                                 VerticalProfile profile)
{
    const auto fixed = criteria(fixtures, profile);
    const auto source = fixtures + frozenProfile(profile).directory + fixed["source"].toString();
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
    const auto savedPage = reopened.doc.pdf().getCatalog()->getPage(2);
    const auto original = readPdf(source);
    const auto originalPage = original.getCatalog()->getPage(2);
    check(savedPage->getUserUnit() == originalPage->getUserUnit() &&
              savedPage->getCropBox() == originalPage->getCropBox() &&
              savedPage->getPageRotation() == originalPage->getPageRotation(),
          "Saved CropBox, rotation and UserUnit retained");
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
QJsonObject testVerticalOcrUi(const QString& fixtures, const QString& output)
{
    return verticalOcrUi(fixtures, output, VerticalProfile::Standard);
}
QJsonObject testVerticalOcrUnitUi(const QString& fixtures, const QString& output)
{
    const auto unitOutput = output + "/vertical-unit";
    check(QDir().mkpath(unitOutput), "Owned vertical unit outputs");
    return verticalOcrUi(fixtures, unitOutput, VerticalProfile::Unit);
}
QJsonObject testVerticalOcrRotationUi(const QString& fixtures, const QString& output,
                                      VerticalProfile profile)
{
    const auto rotationOutput =
        output + (profile == VerticalProfile::Rotate90    ? "/vertical-rotation"
                  : profile == VerticalProfile::Rotate180 ? "/vertical-rotation180"
                                                          : "/vertical-rotation270");
    check(QDir().mkpath(rotationOutput), "Owned vertical rotation outputs");
    return verticalOcrUi(fixtures, rotationOutput, profile);
}
static void cancelVertical(Window& window)
{
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
}
QJsonObject testVerticalGeometryCancel(const QString& fixtures, VerticalProfile profile)
{
    const auto fixed = criteria(fixtures, profile);
    const auto source = fixtures + frozenProfile(profile).directory + fixed["source"].toString();
    Window window;
    window.doc.open(source);
    window.doc.putSignature(2, "取消前署名", {80, 60}, 12, Qt::black);
    const auto before = encodePdf(window.doc.pdf());
    cancelVertical(window);
    window.doc.undo();
    check(signatures(window.doc.pdf(), 2).isEmpty(), "Cancel preserves prior undo action");
    window.doc.redo();
    check(encodePdf(window.doc.pdf()) == before &&
              fileHash(source).toHex() == fixed["source_sha256"].toString().toLatin1(),
          "Cancel preserves redo and frozen input");
    return {{"actual_first_page_progress_cancel", true},
            {"no_partial_UI_commit", true},
            {"private_job_removed", true},
            {"signature_Undo_Redo_source_retained", true},
            {"native_GUI_OS_clipboard", "未実行"}};
}
QJsonObject testVerticalOcrFailure(const QString& fixtures, const QString& output)
{
    const auto fixed = criteria(fixtures);
    Window window;
    window.doc.open(fixtures + "/" + fixed["source"].toString());
    window.doc.putSignature(2, "取消前署名", {80, 60}, 12, Qt::black);
    cancelVertical(window);
    const auto before = encodePdf(window.doc.pdf());
    const auto cursor = window.doc.cursor;
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
    WorkerChannels missingChannels(child, failedJob->path());
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
    error.write(missingChannels.error().toUtf8());
    int rotationsPrepared = 0;
    for (int rotation = 0; rotation < 360; rotation += 90)
    {
        Document rotated;
        rotated.open(fixtures + "/" + fixed["source"].toString());
        rotated.putSignature(2, "保持する署名", {80, 60}, 12, Qt::black);
        for (int angle = 0; angle < rotation; angle += 90)
            rotated.rotate(2);
        const auto retained = encodePdf(rotated.pdf());
        const auto retainedCursor = rotated.cursor;
        auto vertical = OcrJob::prepare(rotated, {"jpn_vert", "1,3"});
        auto horizontal = OcrJob::prepare(rotated, {"jpn+eng", "3"});
        check(bool(vertical) && bool(horizontal) && encodePdf(rotated.pdf()) == retained &&
                  rotated.cursor == retainedCursor,
              "Both OCR modes prepare each quadrant without changing unsaved edits");
        ++rotationsPrepared;
    }
    return {
        {"actual_partial_progress_cancel", true},
        {"missing_model_refused", true},
        {"invalid_model_combination_refused", true},
        {"original_unsaved_state_retained", true},
        {"supported_rotations_prepared_without_mutation", rotationsPrepared},
        {"geometry_success_condition", "0・90・180・270度の実OCRは各別UI試験。旧拒否・FAILを保持"}};
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
    const auto origins =
        makeFont(true, arrObject({PDFObject::createInteger(1),
                                  arrObject({number(-1400), number(210), number(880)})}));
    check(pdfVerticalGlyphOrigin(origins, 1) == QPointF(210, 880) &&
              pdfVerticalGlyphOrigin(origins, 4) == QPointF(500, 0),
          "W2 has individual origins; unspecified origin uses DW2 and horizontal half-width");
    const auto originRange =
        makeFont(true, arrObject({PDFObject::createInteger(2), PDFObject::createInteger(4),
                                  number(-2200), number(100), number(910)}));
    check(pdfVerticalGlyphOrigin(originRange, 2) == QPointF(100, 910) &&
              pdfVerticalGlyphOrigin(originRange, 4) == QPointF(100, 910) &&
              pdfVerticalGlyphOrigin(originRange, 1) == QPointF(350, 0) &&
              pdfVerticalGlyphOrigin(horizontal, 1).isNull(),
          "Range origins and W half-width; horizontal origin unchanged");
    int refused = 0;
    for (const auto& invalid :
         {arrObject(
              {PDFObject::createInteger(65536), arrObject({number(-1000), number(0), number(0)})}),
          arrObject({PDFObject::createInteger(1), arrObject({number(-1000), number(0)})}),
          arrObject({PDFObject::createInteger(4), PDFObject::createInteger(2), number(-1000),
                     number(0), number(0)}),
          arrObject({PDFObject::createInteger(1),
                     arrObject({number(-1000), PDFObject::createName("invalid"), number(0)})})})
        try
        {
            makeFont(true, invalid);
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    check(refused == 4, "Invalid W2 CID/triple/range/origin refuse safely");
    PDFDocumentBuilder glyphBuilder(&document);
    const auto embedded = embedBodyTextFont(glyphBuilder, signatureFont(), "縦");
    auto glyphDocument = glyphBuilder.build();
    auto face = *glyphDocument.getObject(embedded.font).getDictionary();
    const auto descendants = glyphDocument.getObject(face.get("DescendantFonts"));
    auto descendant = *glyphDocument.getObject(descendants.getArray()->getItem(0)).getDictionary();
    const auto encoded = embedded.encodedLines.front();
    check(encoded.size() == 2, "Embedded real glyph code for origin cache test");
    const auto cid = (quint16(quint8(encoded[0])) << 8) | quint16(quint8(encoded[1]));
    const auto originalMapping = glyphDocument.getObject(descendant.get("CIDToGIDMap"));
    const auto gid =
        originalMapping.isStream()
            ? glyphDocument.getDecodedStream(originalMapping.getStream()).mid(cid * 2, 2)
            : encoded;
    check(gid.size() == 2, "Embedded real glyph mapping");
    QByteArray mapping(2, 0);
    for (int i = 0; i < 4; ++i)
        mapping += gid;
    set(descendant, "CIDToGIDMap", streamObject({}, mapping));
    set(descendant, "W2",
        arrObject({PDFObject::createInteger(1),
                   arrObject({number(-1000), number(100), number(800), number(-1000), number(300),
                              number(600), number(-1000), number(100), number(800), number(-1000),
                              number(300), number(600)})}));
    set(face, "Encoding", PDFObject::createName("Identity-V"));
    set(face, "DescendantFonts", arrObject({dictObject(descendant)}));
    const auto cacheFont =
        PDFFont::createFont(dictObject(face), "origin-cache-test", &glyphDocument);
    PDFRenderErrorReporterDummy reporter;
    const auto realized = PDFRealizedFont::createRealizedFont(cacheFont, 100, &reporter);
    TextSequence sequence;
    realized->fillTextSequence(QByteArray::fromHex("0001000200030004"), sequence, &reporter);
    check(sequence.items.size() == 4 && sequence.items[0].glyph && sequence.items[1].glyph &&
              sequence.items[0].glyph == sequence.items[2].glyph &&
              sequence.items[1].glyph == sequence.items[3].glyph &&
              sequence.items[0].glyph != sequence.items[1].glyph,
          "Same embedded GID shares each origin without conflating different origins");
    const auto a = sequence.items[0].glyph->boundingRect();
    const auto b = sequence.items[1].glyph->boundingRect();
    check(!a.isEmpty() && !b.isEmpty() && qAbs(b.x() - a.x() + 20) < 1e-6 &&
              qAbs(b.y() - a.y() - 20) < 1e-6 && a.size() == b.size(),
          "Actual embedded glyph shifts by declared W2 position vectors");
    check(keepVerticalJapaneseSpacing(90, QChar(u'縦'), QChar(u'書')) &&
              keepVerticalJapaneseSpacing(270, QChar(u'す'), QChar(u'。')) &&
              !keepVerticalJapaneseSpacing(0, QChar(u'縦'), QChar(u'書')) &&
              !keepVerticalJapaneseSpacing(90, QChar(u'A'), QChar(u'B')) &&
              !keepVerticalJapaneseSpacing(90, QChar(u'字'), QChar(u' ')),
          "Japanese vertical tracking; horizontal, Latin and encoded spaces retained");
    return {{"individual_range_default_vertical_widths", true},
            {"individual_range_default_vertical_origins", true},
            {"actual_shared_GID_distinct_origin_paths", true},
            {"horizontal_widths_unchanged", true},
            {"invalid_width_refusals", refused}};
}
} // namespace tatsu
