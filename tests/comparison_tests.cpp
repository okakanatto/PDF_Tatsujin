#include "comparison_tests.h"
#include "annotation_operations.h"
#include "bookmark_edit.h"
#include "comparison_dialog.h"
#include "encrypted_pdf.h"
#include "form_fields.h"
#include "link_edit.h"
#include "page_operations.h"
#include "pdf_objects.h"
#include "pdfdocumentreader.h"
#include "pdfencoding.h"
#include "pdfsecurityhandler.h"
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
    fail("Invalid comparison succeeded");
}
PDFDocument changedPage(const PDFDocument& source, const char* key, PDFObject value)
{
    auto storage = source.getStorage();
    const auto ref = source.getCatalog()->getPage(0)->getPageReference();
    auto page = *storage.getObjectByReference(ref).getDictionary();
    detail::set(page, key, std::move(value));
    storage.setObject(ref, detail::dictObject(page));
    return PDFDocument(std::move(storage), source.getInfo()->version, source.getSourceDataHash());
}
PDFDocument title(const PDFDocument& source, QString value)
{
    auto storage = source.getStorage();
    const auto ref = source.getTrailerDictionary()->get("Info").getReference();
    auto info = *storage.getObjectByReference(ref).getDictionary();
    detail::set(info, "Title", PDFObjectFactory::createTextString(value));
    storage.setObject(ref, detail::dictObject(info));
    return PDFDocument(std::move(storage), source.getInfo()->version, source.getSourceDataHash());
}
void saveReport(const ComparisonResult& result, QString path)
{
    exportComparison(result, path);
}
PDFDocument bodyPage(QString text)
{
    QBuffer buffer;
    check(buffer.open(QIODevice::WriteOnly), "Create synthetic body PDF");
    QFont font(signatureFont(), 12);
    {
        QPdfWriter writer(&buffer);
        writer.setResolution(72);
        writer.setPageSize(QPageSize(QPageSize::A4));
        QPainter painter(&writer);
        painter.setFont(font);
        painter.drawText(QRectF(80, 100, 400, 60), text);
        painter.end();
    }
    PDFDocumentReader reader(
        nullptr,
        [](bool* ok)
        {
            *ok = false;
            return QString();
        },
        false, false);
    auto document = reader.readFromBuffer(buffer.data());
    check(reader.getReadingResult() == PDFDocumentReader::Result::OK, "Read synthetic body PDF");
    return correctFontUnicode(document, text, QRawFont::fromFont(font));
}
PDFDocument latinPage(const PDFDocument& source, int characters)
{
    auto storage = source.getStorage();
    PDFDictionary font;
    detail::set(font, "Type", PDFObject::createName("Font"));
    detail::set(font, "Subtype", PDFObject::createName("Type1"));
    detail::set(font, "BaseFont", PDFObject::createName("Helvetica"));
    detail::set(font, "Encoding", PDFObject::createName("WinAnsiEncoding"));
    const auto fontRef = storage.addObject(detail::dictObject(font));
    PDFDictionary fonts, resources;
    detail::set(fonts, "TestFont", PDFObject::createReference(fontRef));
    detail::set(resources, "Font", detail::dictObject(fonts));
    const auto ref = source.getCatalog()->getPage(0)->getPageReference();
    auto page = *storage.getObjectByReference(ref).getDictionary();
    detail::set(page, "Resources", detail::dictObject(resources));
    const auto contents = storage.addObject(detail::streamObject(
        {}, "BT /TestFont 6 Tf 20 20 Td (" + QByteArray(characters, 'A') + ") Tj ET"));
    detail::set(page, "Contents", PDFObject::createReference(contents));
    storage.setObject(ref, detail::dictObject(page));
    return PDFDocument(std::move(storage), source.getInfo()->version, source.getSourceDataHash());
}
PDFDocument repeatPages(const PDFDocument& source, int count)
{
    auto storage = source.getStorage();
    auto page = *storage.getObjectByReference(source.getCatalog()->getPage(0)->getPageReference())
                     .getDictionary();
    const auto parent = page.get("Parent").getReference();
    std::vector<PDFObject> kids;
    for (int i = 0; i < count; ++i)
        kids.push_back(PDFObject::createReference(storage.addObject(detail::dictObject(page))));
    auto pages = *storage.getObjectByReference(parent).getDictionary();
    detail::set(pages, "Count", PDFObject::createInteger(count));
    detail::set(pages, "Kids", detail::arrObject(kids));
    storage.setObject(parent, detail::dictObject(pages));
    return PDFDocument(std::move(storage), source.getInfo()->version, source.getSourceDataHash());
}
PDFDocument protectedFile(const PDFDocument& source, const QString& path, bool copy)
{
    EncryptionOptions options{"compare-user-2026", "compare-owner-2026"};
    options.copy = copy;
    exportProtectedPdf(source, options, path);
    return readPdf(path, options.userPassword);
}
void pairs(const ComparisonResult& result, QVector<QPair<int, int>> expected)
{
    check(result.pages.size() == expected.size(), "Exact expected page-pair count");
    for (int index = 0; index < expected.size(); ++index)
        check(result.pages[index].left == expected[index].first &&
                  result.pages[index].right == expected[index].second,
              "Exact expected common-page correspondence");
}
} // namespace
QJsonObject testComparisonLifecycle(const QString& fixtures, const QString& output)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    const auto bytes = encodePdf(source), sourceHash = fileHash(fixtures + "/D02.pdf");
    auto equal = compareDocuments(source, source);
    check(equal.same(), "Identical document is equal in compared scope");
    writeCandidate(source, output + "/comparison-same-before.pdf");
    writeCandidate(source, output + "/comparison-same-after.pdf");
    check(compareDocuments(source, readPdf(output + "/comparison-same-after.pdf")).same(),
          "Ordinary reserialization has no false content change");
    saveReport(equal, output + "/comparison-same.json");
    auto bodyBefore = bodyPage("比較する本文：確認前"),
         bodyAfter = bodyPage("比較する本文：確認後");
    auto body = compareDocuments(bodyBefore, bodyAfter);
    check(body.changedPages() == 1 && body.pages[0].textChanged && body.pages[0].pixelsChanged &&
              body.pages[0].leftText.contains("確認前") &&
              body.pages[0].rightText.contains("確認後"),
          "Actual Japanese body text change detected");
    writeCandidate(bodyBefore, output + "/comparison-body-before.pdf");
    writeCandidate(bodyAfter, output + "/comparison-body-after.pdf");
    saveReport(body, output + "/comparison-body.json");
    Document signature;
    signature.history = {source};
    signature.saved = -1;
    signature.putSignature(0, "比較の日本語署名", {100, 200}, 12, Qt::blue);
    const auto cursor = signature.cursor, saved = signature.saved;
    const auto revision = signature.revision;
    auto changed = compareDocuments(source, signature.pdf());
    check(changed.changedPages() == 1 && changed.pages[0].annotationsChanged &&
              changed.pages[0].pixelsChanged && !changed.pages[0].pixelBounds.isEmpty(),
          "Japanese signature visible change detected exactly on page 1");
    writeCandidate(source, output + "/comparison-signature-before.pdf");
    writeCandidate(signature.pdf(), output + "/comparison-signature-after.pdf");
    saveReport(changed, output + "/comparison-signature.json");
    check(signature.cursor == cursor && signature.saved == saved &&
              signature.revision == revision && signature.dirty(),
          "Comparison keeps document history and unsaved state");
    Document comment;
    comment.history = {source};
    auto mark =
        putAnnotation(comment, 0, OverlayKind::Comment, {60, 300, 24, 24}, "確認前", Qt::red, 1.5);
    const auto beforeComment = comment.pdf();
    auto storage = comment.pdf().getStorage();
    auto annotation = *storage.getObjectByReference(mark.ref).getDictionary();
    detail::set(annotation, "Contents", PDFObjectFactory::createTextString("確認後"));
    storage.setObject(mark.ref, detail::dictObject(annotation));
    auto afterComment = PDFDocument(std::move(storage), beforeComment.getInfo()->version,
                                    beforeComment.getSourceDataHash());
    auto note = compareDocuments(beforeComment, afterComment);
    check(note.changedPages() == 1 && note.pages[0].annotationsChanged &&
              !note.pages[0].pixelsChanged,
          "Invisible comment text change detected without invented pixel changes");
    writeCandidate(beforeComment, output + "/comparison-comment-before.pdf");
    writeCandidate(afterComment, output + "/comparison-comment-after.pdf");
    saveReport(note, output + "/comparison-comment.json");
    Document form;
    form.open(fixtures + "/D07.pdf");
    auto originalForm = form.pdf();
    bool wrote = false;
    for (const auto& field : formFields(form.pdf()))
        if (field.qualifiedName == "name")
        {
            putFormValue(form, field.widget, {"髙橋 香織"});
            wrote = true;
        }
    auto values = compareDocuments(originalForm, form.pdf());
    check(wrote && values.changedPages() == 1 && values.pages[0].formsChanged,
          "Standard Japanese form values compared");
    writeCandidate(originalForm, output + "/comparison-form-before.pdf");
    writeCandidate(form.pdf(), output + "/comparison-form-after.pdf");
    saveReport(values, output + "/comparison-form.json");
    auto withTitle = title(source, "比較対象の新しい題名");
    auto info = compareDocuments(source, withTitle);
    check(info.changedPages() == 0 && info.propertyChanges == QStringList{"title"},
          "Metadata-only title change distinguished from page changes");
    writeCandidate(withTitle, output + "/comparison-title-after.pdf");
    saveReport(info, output + "/comparison-title.json");
    auto outlines = replaceBookmarks(source, {{{}, "比較用しおり", -1, 2, true, true}});
    auto bookmark = compareDocuments(source, outlines);
    check(bookmark.changedPages() == 0 && bookmark.propertyChanges.contains("bookmarks"),
          "Bookmark-only change detected");
    writeCandidate(outlines, output + "/comparison-bookmark-after.pdf");
    saveReport(bookmark, output + "/comparison-bookmark.json");
    auto links = editableLinks(source);
    links << LinkEntry{0, -1, {20, 20, 120, 35}, "別ページへ", LinkTarget::Page, 1, {}};
    auto linked = replaceLinks(source, links);
    links = editableLinks(linked);
    links.last().target = LinkTarget::Page;
    links.last().destination = 2;
    auto relinked = replaceLinks(linked, links);
    auto link = compareDocuments(linked, relinked);
    check(link.changedPages() == 1 && link.pages[0].annotationsChanged &&
              !link.pages[0].pixelsChanged,
          "Invisible link destination change detected");
    writeCandidate(linked, output + "/comparison-link-before.pdf");
    writeCandidate(relinked, output + "/comparison-link-after.pdf");
    saveReport(link, output + "/comparison-link.json");
    Document image;
    image.history = {source};
    QImage pixels(80, 50, QImage::Format_RGB32);
    pixels.fill(QColor("#59a3ed"));
    image.putImage(0, OverlayKind::Image, pixels, {100, 350}, 80);
    auto bitmap = compareDocuments(source, image.pdf());
    check(bitmap.changedPages() == 1 && bitmap.pages[0].pixelsChanged, "Added bitmap detected");
    writeCandidate(image.pdf(), output + "/comparison-image-after.pdf");
    saveReport(bitmap, output + "/comparison-image.json");
    Document shape;
    shape.history = {source};
    putAnnotation(shape, 0, OverlayKind::Rectangle, {110, 300, 100, 50}, "比較の枠", Qt::blue, 2);
    auto vector = compareDocuments(source, shape.pdf());
    check(vector.changedPages() == 1 && vector.pages[0].pixelsChanged &&
              vector.pages[0].annotationsChanged,
          "Vector annotation detected");
    writeCandidate(shape.pdf(), output + "/comparison-vector-after.pdf");
    saveReport(vector, output + "/comparison-vector.json");
    QJsonArray geometries;
    for (const auto& entry : QVector<QPair<QByteArray, PDFObject>>{
             {"Rotate", PDFObject::createInteger(90)},
             {"CropBox", detail::rectObject(QRectF(25, 35, 420, 520))},
             {"UserUnit", detail::number(1.5)}})
    {
        const auto right = changedPage(source, entry.first.constData(), entry.second);
        auto result = compareDocuments(source, right, {false});
        check(result.changedPages() == 1 && result.pages[0].geometryChanged,
              "CropBox, rotation or UserUnit change detected");
        const auto stem = "comparison-geometry-" + QString::fromLatin1(entry.first);
        writeCandidate(right, output + "/" + stem + ".pdf");
        saveReport(result, output + "/" + stem + ".json");
        geometries << stem;
    }
    const auto additional = readPdf(fixtures + "/D01.pdf");
    auto inserted = insertPages(source, additional, {0}, 2);
    auto added = compareDocuments(source, inserted);
    pairs(added, {{0, 0}, {1, 1}, {-1, 2}, {2, 3}, {3, 4}});
    check(added.changedPages() == 1, "Inserted page does not shift later comparisons");
    writeCandidate(inserted, output + "/comparison-inserted.pdf");
    saveReport(added, output + "/comparison-inserted.json");
    auto removed = selectPages(source, {0, 2, 3});
    auto deleted = compareDocuments(source, removed);
    pairs(deleted, {{0, 0}, {1, -1}, {2, 1}, {3, 2}});
    check(deleted.changedPages() == 1, "Deleted page correspondence");
    writeCandidate(removed, output + "/comparison-deleted.pdf");
    saveReport(deleted, output + "/comparison-deleted.json");
    auto numbered = compareDocuments(source, inserted, {false});
    pairs(numbered, {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {-1, 4}});
    check(numbered.changedPages() == 3, "Explicit page-number mode compares shown numbers");
    saveReport(numbered, output + "/comparison-numbered.json");
    auto repeated = mergeDocuments({additional, additional});
    auto repeatedResult = compareDocuments(repeated, repeated);
    check(repeatedResult.same() && repeatedResult.pages.size() == 2,
          "Repeated identical pages have stable order");
    check(encodePdf(source) == bytes && fileHash(fixtures + "/D02.pdf") == sourceHash,
          "All comparisons keep original memory and file");
    return {{"same_resave_no_false_change", true},
            {"signature_comment_form_link_bookmark_info", true},
            {"geometry_cases", geometries},
            {"exact_page_pairs", true},
            {"input_history_unchanged", true}};
}
QJsonObject testComparisonFailures(const QString& fixtures, const QString& output)
{
    auto source = readPdf(fixtures + "/D01.pdf");
    const auto original = encodePdf(source);
    QJsonArray rows;
    auto invalid = [&](QString name, const std::function<void()>& operation, QString expected = {})
    {
        const auto diagnostic = output + "/comparison-case-progress.json";
        auto record = [&](const char* state)
        {
            QFile file(diagnostic);
            check(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                  "Record comparison test progress");
            file.write(QJsonDocument(QJsonObject{{"case", name}, {"state", state}}).toJson());
        };
        record("started");
        const auto error = rejects(operation);
        check(expected.isEmpty() || error.contains(expected),
              "Expected error category for " + name);
        rows << QJsonObject{{"case", name}, {"error", error}};
        check(encodePdf(source) == original, "Rejected comparison preserves source");
        record("completed");
    };
    invalid("empty document", [&] { compareDocuments(source, PDFDocument{}); });
    auto denied = protectedFile(source, output + "/comparison-copy-denied.pdf", false);
    invalid("copy prohibited", [&] { compareDocuments(source, denied); });
    auto allowed = protectedFile(source, output + "/comparison-copy-allowed.pdf", true);
    check(compareDocuments(allowed, allowed).same(),
          "Permitted encrypted document comparison is read-only");
    const auto ownerRole = allowed.getStorage().getSecurityHandler()->getAuthorizationResult();
    check(ownerRole == PDFSecurityHandler::AuthorizationResult::UserAuthorized,
          "Comparison does not promote user to owner");
    invalid("wrong password", [&] { readPdf(output + "/comparison-copy-allowed.pdf", "wrong"); });
    invalid("broken PDF", [&] { readPdf(fixtures + "/D08-broken.pdf"); });
    auto storage = source.getStorage();
    auto page = *storage.getObjectByReference(source.getCatalog()->getPage(0)->getPageReference())
                     .getDictionary();
    const auto parent = page.get("Parent").getReference();
    QJsonArray dummy;
    std::vector<PDFObject> kids;
    for (int i = 0; i < 501; ++i)
        kids.push_back(PDFObject::createReference(storage.addObject(detail::dictObject(page))));
    auto pages = *storage.getObjectByReference(parent).getDictionary();
    detail::set(pages, "Count", PDFObject::createInteger(501));
    detail::set(pages, "Kids", detail::arrObject(kids));
    storage.setObject(parent, detail::dictObject(pages));
    auto tooMany =
        PDFDocument(std::move(storage), source.getInfo()->version, source.getSourceDataHash());
    invalid("501 pages", [&] { compareDocuments(source, tooMany); });
    auto huge = changedPage(source, "MediaBox", detail::rectObject(QRectF(0, 0, 4001, 4001)));
    huge = changedPage(huge, "CropBox", detail::rectObject(QRectF(0, 0, 4001, 4001)));
    invalid("over 16 million pixels", [&] { compareDocuments(source, huge); });
    invalid("metadata over 65536 characters",
            [&] { compareDocuments(source, title(source, QString(65537, 'x'))); });
    invalid(
        "over 200000 extracted characters",
        [&] { compareDocuments(source, latinPage(source, 200001)); }, "抽出文字");
    const auto manyText = repeatPages(latinPage(source, 99999), 51);
    invalid(
        "over 10 million total extracted characters", [&] { compareDocuments(manyText, manyText); },
        "抽出文字");
    auto infoStorage = source.getStorage();
    const auto infoRef = source.getTrailerDictionary()->get("Info").getReference();
    auto largeInfo = *infoStorage.getObjectByReference(infoRef).getDictionary();
    for (int i = 0; i < 80; ++i)
        largeInfo.setEntry(
            PDFInplaceOrMemoryString(("TatsuExtra" + QByteArray::number(i)).constData()),
            PDFObjectFactory::createTextString(QString(65000, 'x')));
    infoStorage.setObject(infoRef, detail::dictObject(largeInfo));
    auto extraInfo =
        PDFDocument(std::move(infoStorage), source.getInfo()->version, source.getSourceDataHash());
    invalid("over 4MiB document properties", [&] { compareDocuments(source, extraInfo); }, "4MiB");
    Document noted;
    noted.history = {source};
    auto mark = noted.putSignature(0, "重複注釈", {100, 200}, 12, Qt::black);
    std::vector<PDFObject> duplicates(10001, PDFObject::createReference(mark.ref));
    auto tooManyNotes = changedPage(noted.pdf(), "Annots", detail::arrObject(duplicates));
    invalid("10001 annotations", [&] { compareDocuments(source, tooManyNotes); });
    auto notesStorage = noted.pdf().getStorage();
    auto longNote = *notesStorage.getObjectByReference(mark.ref).getDictionary();
    detail::set(longNote, "Contents", PDFObjectFactory::createTextString(QString(65536, 'x')));
    notesStorage.setObject(mark.ref, detail::dictObject(longNote));
    auto notes = PDFDocument(std::move(notesStorage), noted.pdf().getInfo()->version,
                             noted.pdf().getSourceDataHash());
    auto largeNotes = changedPage(
        notes, "Annots",
        detail::arrObject(std::vector<PDFObject>(600, PDFObject::createReference(mark.ref))));
    invalid(
        "over 32MiB semantic comparison data", [&] { compareDocuments(source, largeNotes); },
        "32MiB");
    invalid("initial cancellation",
            [&] { compareDocuments(source, source, {}, [] { return true; }); });
    bool cancelled = false;
    int observed = 0;
    auto multi = readPdf(fixtures + "/D02.pdf");
    invalid("cancel after actual page measurement",
            [&]
            {
                compareDocuments(
                    multi, multi, {}, [&] { return cancelled; },
                    [&](QString, int done, int)
                    {
                        observed = done;
                        if (done == 1)
                            cancelled = true;
                    });
            });
    check(cancelled && observed == 1, "Real progress before cancellation");
    auto result = compareDocuments(source, source);
    const auto existing = output + "/comparison-existing.json";
    saveReport(result, existing);
    const auto hash = fileHash(existing);
    invalid("existing output", [&] { exportComparison(result, existing); });
    check(fileHash(existing) == hash, "Existing report kept");
    ComparisonResult tooLarge = result;
    tooLarge.pages[0].leftText = QString(64 * 1024 * 1024 + 1, 'x');
    invalid(
        "over 64MiB result JSON",
        [&] { exportComparison(tooLarge, output + "/comparison-too-large.json"); }, "64MiB");
    check(!QFileInfo::exists(output + "/comparison-too-large.json"),
          "Oversized result never published");
    invalid("invalid folder", [&] { exportComparison(result, output + "/missing/report.json"); });
    invalid("wrong extension", [&] { exportComparison(result, output + "/report.pdf"); });
    int calls = 0;
    invalid("cancel after writing report",
            [&] {
                exportComparison(result, output + "/comparison-cancelled.json",
                                 [&] { return ++calls == 2; });
            });
    check(calls == 2 && !QFileInfo::exists(output + "/comparison-cancelled.json"),
          "Written candidate discarded");
    const auto race = output + "/comparison-race.json";
    calls = 0;
    invalid("actual publication conflict",
            [&]
            {
                exportComparison(result, race,
                                 [&]
                                 {
                                     if (++calls == 3)
                                     {
                                         QFile file(race);
                                         check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                                               "Create competing report");
                                         file.write("KEEP-COMPARE-COLLISION");
                                     }
                                     return false;
                                 });
            });
    QFile collision(race);
    check(collision.open(QIODevice::ReadOnly) && collision.readAll() == "KEEP-COMPARE-COLLISION",
          "Competing report retained");
    const auto deniedFolder = qEnvironmentVariable("TATSU_DENIED_SAVE_DIR");
    if (!deniedFolder.isEmpty())
        invalid("NTFS write denial",
                [&] { exportComparison(result, deniedFolder + "/comparison.json"); });
    return {{"rejected", rows},
            {"encrypted_role_unchanged", true},
            {"NTFS_denial", deniedFolder.isEmpty() ? "未実行" : "PASS"},
            {"volume_full", "未実行"}};
}
QJsonObject testComparisonUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.open(fixtures + "/D02.pdf");
    window.doc.putSignature(0, "比較前の未保存署名", {100, 200}, 12, Qt::blue);
    window.refresh(true);
    window.show();
    const auto snapshot = window.doc.pdf();
    const auto cursor = window.doc.cursor;
    const auto revision = window.doc.revision;
    const auto originalHash = fileHash(window.doc.source);
    Document right;
    right.history = {snapshot};
    right.rotate(1);
    right.commit(insertPages(right.pdf(), readPdf(fixtures + "/D01.pdf"), {0}, 2));
    const auto target = output + "/comparison-ui-right.pdf";
    writeCandidate(right.pdf(), target);
    int mode = 0, ticks = 0;
    QString error;
    auto operate = [&]
    {
        int state = 0;
        QElapsedTimer deadline;
        deadline.start();
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(
            &timer, &QTimer::timeout,
            [&]
            {
                ++ticks;
                auto dialog = dynamic_cast<ComparisonDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                try
                {
                    check(deadline.elapsed() < 45000, "Comparison UI deadline");
                    auto start = dialog->findChild<QPushButton*>("comparisonStart");
                    auto save = dialog->findChild<QPushButton*>("comparisonExport");
                    auto path = dialog->findChild<QLineEdit*>("comparisonPath");
                    if (state == 0)
                    {
                        dialog->resize(800, 480);
                        check(dialog->findChild<QLineEdit*>("comparisonPassword")->echoMode() ==
                                  QLineEdit::Password,
                              "Comparison password masked");
                        path->setText(output + "/missing.pdf");
                        QTest::mouseClick(start, Qt::LeftButton);
                        state = 1;
                    }
                    else if (state == 1 && start->isEnabled())
                    {
                        check(!dialog->comparison() && !save->isEnabled(),
                              "Failed read does not show old results");
                        path->setText(target);
                        QTest::mouseClick(start, Qt::LeftButton);
                        state = 2;
                        if (mode == 1)
                        {
                            bool running = false;
                            for (auto thread : dialog->findChildren<QThread*>())
                                running |= thread->isRunning();
                            check(running, "Actual owned comparison worker started");
                            QTest::mouseClick(dialog->findChild<QPushButton*>("comparisonCancel"),
                                              Qt::LeftButton);
                            state = 8;
                        }
                    }
                    else if (state == 2 && dialog->comparison() && start->isEnabled())
                    {
                        check(dialog->comparison()->changedPages() == 2,
                              "Comparison UI fixed changed and inserted page count");
                        auto table = dialog->findChild<QTableWidget*>("comparisonPages");
                        table->selectRow(1);
                        state = 3;
                    }
                    else if (state == 3)
                    {
                        auto left = dialog->findChild<QGraphicsView*>("comparisonLeftPreview");
                        auto rightView =
                            dialog->findChild<QGraphicsView*>("comparisonRightPreview");
                        if (left->scene()->items().isEmpty() ||
                            rightView->scene()->items().isEmpty())
                            return;
                        check(left->scene()->items().size() >= 2 &&
                                  rightView->scene()->items().size() >= 2,
                              "Actual two PDF previews and changed-area frames");
                        dialog->findChild<QComboBox*>("comparisonZoom")->setCurrentIndex(2);
                        check(std::abs(left->transform().m11() - 1.5) < 1e-6 &&
                                  std::abs(rightView->transform().m11() - 1.5) < 1e-6,
                              "Both previews zoom consistently");
                        dialog->findChild<QComboBox*>("comparisonZoom")->setCurrentIndex(0);
                        dialog->grab().save(output + "/comparison-ui.png");
                        check(!start->visibleRegion().isEmpty() &&
                                  !save->visibleRegion().isEmpty() &&
                                  !dialog->findChild<QPushButton*>("comparisonCancel")
                                       ->visibleRegion()
                                       .isEmpty(),
                              "Comparison actions visible at compact dimensions");
                        dialog->findChild<QLineEdit*>("comparisonOutput")
                            ->setText(output + "/comparison-ui-result.json");
                        QTest::mouseClick(save, Qt::LeftButton);
                        state = 4;
                    }
                    else if (state == 4 && start->isEnabled())
                    {
                        check(QFileInfo::exists(output + "/comparison-ui-result.json"),
                              "UI writes actual report");
                        QFile changed(target);
                        check(changed.open(QIODevice::Append), "Modify actual comparison input");
                        changed.write("\n% changed after comparison\n");
                        changed.close();
                        dialog->findChild<QLineEdit*>("comparisonOutput")
                            ->setText(output + "/comparison-ui-stale.json");
                        QTest::mouseClick(save, Qt::LeftButton);
                        state = 5;
                    }
                    else if (state == 5 && start->isEnabled())
                    {
                        check(!dialog->comparison() && !save->isEnabled() &&
                                  !QFileInfo::exists(output + "/comparison-ui-stale.json"),
                              "Input update invalidates report and output");
                        dialog->reject();
                    }
                    else if (state == 8 && start->isEnabled())
                    {
                        check(!dialog->comparison() && !save->isEnabled(),
                              "Cancelled comparison has no partial result");
                        dialog->reject();
                    }
                }
                catch (const std::exception& exception)
                {
                    error = QString::fromUtf8(exception.what());
                    dialog->reject();
                }
            });
        timer.start(3);
        window.compareWithDocument();
        timer.stop();
        check(error.isEmpty(), error);
        check(window.doc.pdf() == snapshot && window.doc.cursor == cursor &&
                  window.doc.revision == revision && window.doc.dirty() &&
                  window.doc.target.isEmpty() && fileHash(window.doc.source) == originalHash,
              "Actual comparison UI preserves source and Undo");
    };
    operate();
    mode = 1;
    operate();
    window.undoAction->trigger();
    check(window.doc.cursor == cursor - 1, "Original Undo still works");
    window.redoAction->trigger();
    check(window.doc.pdf() == snapshot, "Original Redo still works");
    window.doc.open(output + "/comparison-copy-denied.pdf", "compare-user-2026");
    window.refresh(true);
    auto action = window.findChild<QAction*>("compareDocuments");
    check(action && !action->isEnabled(), "Product disables comparison when copy is denied");
    return {{"actual_read_retry_compare_two_previews_zoom_export", true},
            {"input_change_invalidation", true},
            {"owned_worker_cancel", true},
            {"original_Undo_Redo", true},
            {"GUI_event_ticks", ticks},
            {"native_UI", "未実行"}};
}
QJsonObject testComparisonOcr(const QString& fixtures, const QString& output)
{
    Window window;
    window.doc.history = {selectPages(readPdf(fixtures + "/D03.pdf"), {2, 6})};
    window.doc.saved = -1;
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
          "Actual bilingual OCR before comparison");
    check(error.isEmpty(), error);
    auto original = window.doc.pdf();
    auto identical = compareDocuments(original, original);
    check(identical.same() && identical.pages[0].leftText.contains("市民公園") &&
              identical.pages[1].leftText.contains("coastal", Qt::CaseInsensitive),
          "Fixed OCR terms compared and retained");
    writeCandidate(original, output + "/comparison-ocr-before.pdf");
    window.doc.putSignature(0, "OCR後の比較変更", {30, 30}, 10, Qt::blue);
    auto changed = compareDocuments(original, window.doc.pdf());
    check(changed.changedPages() == 1 && !changed.pages[0].textChanged &&
              changed.pages[0].pixelsChanged && changed.pages[0].annotationsChanged,
          "OCR body remains exact when signature changes");
    writeCandidate(window.doc.pdf(), output + "/comparison-ocr-after.pdf");
    saveReport(changed, output + "/comparison-ocr.json");
    window.doc.undo();
    window.doc.redo();
    return {{"actual_bilingual_OCR", true},
            {"fixed_search_terms", true},
            {"signature_change_without_OCR_loss", true},
            {"input_Undo_Redo", true}};
}
} // namespace tatsu
