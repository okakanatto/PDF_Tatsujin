#include "pdf_text_docx_tests.h"
#include "office_import.h"
#include "office_import_tests.h"
#include "pdf_text_docx.h"
#include "pdf_text_docx_dialog.h"
#include "window.h"
#include <QXmlStreamReader>
#include <QtCore/private/qzipreader_p.h>
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool condition, const QString& why)
{
    if (!condition)
        fail(why);
}
QJsonObject fixed(const QString& fixtures)
{
    QFile file(fixtures + "/pdf-text-docx/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read fixed Word criterion");
    const auto bytes = file.readAll();
    check(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() ==
              "52a3dbd5e25b943939066defc94e51840d7601cb392b75c9bd5df63115d570f9",
          "Word criterion unchanged");
    const auto result = QJsonDocument::fromJson(bytes).object();
    for (const auto& name : {QString("source"), QString("image_only"), QString("copy_restricted")})
        check(fileHash(fixtures + "/pdf-text-docx/" + result[name].toString()).toHex() ==
                  result[name + "_sha256"].toString().toLatin1(),
              "Frozen Word input: " + name);
    return result;
}
} // namespace
QJsonObject testWordTextCore(const QString& fixtures, const QString& output)
{
    const auto criterion = fixed(fixtures);
    Document document;
    document.open(fixtures + "/pdf-text-docx/" + criterion["source"].toString());
    const auto sourceHash = fileHash(document.source);
    const auto original = encodePdf(document.pdf());
    const auto texts = extractWordText(document.pdf(), {0, 1});
    const auto expected = criterion["expected_pages"].toArray();
    check(texts.size() == expected.size(), "Both Word pages extracted");
    for (int page = 0; page < texts.size(); ++page)
    {
        QStringList lines;
        for (const auto& line : expected[page].toArray())
            lines << line.toString();
        check(texts[page] == lines.join('\n'), "Fixed page text, symbols and reading order");
    }
    const auto target = output + "/word-text.docx";
    exportWordText(texts, target, "Meiryo UI");
    QZipReader zip(target);
    check(zip.status() == QZipReader::NoError && zip.fileInfoList().size() == 3,
          "Actual minimal Word package");
    QXmlStreamReader xml(zip.fileData("word/document.xml"));
    QString extracted;
    int breaks = 0, fonts = 0;
    while (!xml.atEnd())
    {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == "t")
            extracted += xml.readElementText();
        if (xml.name() == "br")
            ++breaks;
        if (xml.name() == "rFonts")
        {
            check(xml.attributes().value("w:eastAsia") == "Meiryo UI", "Named editable Word font");
            ++fonts;
        }
    }
    check(!xml.hasError() && breaks == 1 && fonts >= 7 && extracted.contains("00123 & <文字>"),
          "XML literal text and explicit page boundary");
    check(encodePdf(document.pdf()) == original && fileHash(document.source) == sourceHash,
          "Word export leaves original PDF unchanged");
    auto edited = texts;
    edited[0].replace("00123", "00456");
    exportWordText(edited, output + "/word-text-edited.docx", "Arial");
    return {{"exact_two_page_text", true},
            {"actual_DOCX_and_edited_copy", true},
            {"original_PDF_unchanged", true},
            {"Word_native_GUI", "未実行"}};
}
QJsonObject testWordTextFailures(const QString& fixtures, const QString& output)
{
    const auto criterion = fixed(fixtures);
    Document document;
    document.open(fixtures + "/pdf-text-docx/" + criterion["source"].toString());
    document.putSignature(0, "保持する未保存署名", {80, 120}, 12, Qt::black);
    const auto retained = encodePdf(document.pdf());
    const auto cursor = document.cursor;
    const auto text = extractWordText(document.pdf(), {0});
    const auto sentinel = output + "/word-existing.docx";
    QFile file(sentinel);
    check(file.open(QIODevice::WriteOnly), "Owned Word conflict sentinel");
    file.write("keep original Word bytes");
    file.close();
    const auto sentinelHash = fileHash(sentinel);
    int rejected = 0;
    const auto refuse = [&](auto operation)
    {
        bool refused = false;
        try
        {
            operation();
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        check(refused, "Word invalid/cancelled request refuses");
        ++rejected;
    };
    refuse([&] { exportWordText(text, sentinel, "Arial"); });
    refuse([&] { exportWordText(text, output + "/word-wrong.xlsx", "Arial"); });
    refuse([&] { exportWordText({}, output + "/word-empty.docx", "Arial"); });
    refuse([&] { exportWordText({QString(QChar(1))}, output + "/word-control.docx", "Arial"); });
    refuse([&]
           { exportWordText({QString(QChar(0xd800))}, output + "/word-surrogate.docx", "Arial"); });
    refuse(
        [&]
        { exportWordText(text, output + "/word-cancelled.docx", "Arial", [] { return true; }); });
    refuse([&]
           { exportWordText({QString(1000001, 'x')}, output + "/word-too-large.docx", "Arial"); });
    refuse([&] { exportWordText(text, output + "/missing-folder/word.docx", "Arial"); });
    const auto stagingBefore =
        QDir(output).entryList({"PDFTatsujin-office-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    const auto completedCandidate = [&]
    {
        for (const auto& name :
             QDir(output).entryList({"PDFTatsujin-office-*"}, QDir::Dirs | QDir::NoDotAndDotDot))
        {
            QZipReader candidate(output + '/' + name + "/candidate.docx");
            if (candidate.status() == QZipReader::NoError && candidate.fileInfoList().size() == 3)
                return true;
        }
        return false;
    };
    bool cancelledAfterWrite = false;
    const auto lateCancel = output + "/word-cancel-after-write.docx";
    refuse(
        [&]
        {
            exportWordText(text, lateCancel, "Arial",
                           [&]
                           {
                               cancelledAfterWrite = completedCandidate();
                               return cancelledAfterWrite;
                           });
        });
    check(cancelledAfterWrite && !QFileInfo::exists(lateCancel),
          "Cancel after complete DOCX staging publishes no partial output");
    const auto race = output + "/word-publication-race.docx";
    bool competitorCreated = false;
    refuse(
        [&]
        {
            exportWordText(text, race, "Arial",
                           [&]
                           {
                               if (!competitorCreated && completedCandidate())
                               {
                                   QFile competitor(race);
                                   check(competitor.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                                         "Owned competing Word writer");
                                   competitor.write("concurrent-word-sentinel");
                                   competitor.close();
                                   competitorCreated = true;
                               }
                               return false;
                           });
        });
    QFile competitor(race);
    check(competitorCreated && competitor.open(QIODevice::ReadOnly) &&
              competitor.readAll() == "concurrent-word-sentinel",
          "Word publication race preserves competing writer bytes");
    check(QDir(output).entryList({"PDFTatsujin-office-*"}, QDir::Dirs | QDir::NoDotAndDotDot) ==
              stagingBefore,
          "Word failed staging directories removed");
    refuse([&] { extractWordText(document.pdf(), {}); });
    refuse([&] { extractWordText(document.pdf(), {99}); });
    refuse([&] { extractWordText(document.pdf(), {0}, [] { return true; }); });
    for (const auto& name : {QString("image_only"), QString("copy_restricted")})
    {
        const auto source = readPdf(fixtures + "/pdf-text-docx/" + criterion[name].toString());
        refuse([&] { extractWordText(source, {0}); });
    }
    check(fileHash(sentinel) == sentinelHash && encodePdf(document.pdf()) == retained &&
              document.cursor == cursor && document.dirty(),
          "Word refusal retains destination, unsaved signature and Undo");
    return {{"refused_cases", rejected},
            {"late_cancel_and_publication_race_retained", true},
            {"original_unsaved_state_retained", true}};
}
QJsonObject testWordTextUi(const QString& fixtures, const QString& output)
{
    const auto criterion = fixed(fixtures);
    Window window;
    window.show();
    window.openFile(fixtures + "/pdf-text-docx/" + criterion["source"].toString());
    window.doc.putSignature(1, "保持する署名", {80, 250}, 12, Qt::black);
    window.refresh();
    const auto original = encodePdf(window.doc.pdf());
    const auto cursor = window.doc.cursor;
    const auto sourceHash = fileHash(window.doc.source);
    const auto outputPath = output + "/word-ui-edited.docx";
    QString error;
    bool operated = false;
    QTimer automation;
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(
        &automation, &QTimer::timeout, &window,
        [&]
        {
            auto dialog = dynamic_cast<PdfTextDocxDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto save = dialog->findChild<QPushButton*>("wordTextSave");
            if (!save->isEnabled() && elapsed.elapsed() < 30000)
                return;
            automation.stop();
            try
            {
                check(save->isEnabled(), "Word UI async extraction ready");
                auto editor = dialog->findChild<QPlainTextEdit*>("wordTextEditor");
                auto path = dialog->findChild<QLineEdit*>("wordTextPath");
                auto message = dialog->findChild<QLabel*>("wordTextMessage");
                check(editor->find("00123"), "Editable original Word text found");
                editor->setFocus();
                QTest::keyClicks(editor, "00456");
                check(editor->toPlainText().contains("00456 & <文字>"),
                      "Actual Word text keyboard edit");
                auto range = dialog->findChild<QLineEdit*>("wordTextRange");
                range->setText("1-2");
                check(!save->isEnabled(), "Unapplied Word page range cannot save stale text");
                bool declined = false;
                QTimer decline;
                QObject::connect(
                    &decline, &QTimer::timeout, dialog,
                    [&]
                    {
                        auto question =
                            dynamic_cast<QMessageBox*>(QApplication::activeModalWidget());
                        if (question)
                        {
                            decline.stop();
                            declined = true;
                            QTest::mouseClick(question->button(QMessageBox::No), Qt::LeftButton);
                        }
                    });
                decline.start(20);
                range->setFocus();
                QTest::keyClick(range, Qt::Key_Return);
                decline.stop();
                check(declined && range->text() == "1" && save->isEnabled() &&
                          editor->toPlainText().contains("00456 & <文字>"),
                      "Declined re-extraction keeps edited text and applied page scope");
                path->setText(outputPath);
                QTest::mouseClick(save, Qt::LeftButton);
                check(QFileInfo::exists(outputPath) && message->text().startsWith("保存しました"),
                      "Actual Word UI new save");
                const auto savedHash = fileHash(outputPath);
                QTest::mouseClick(save, Qt::LeftButton);
                check(fileHash(outputPath) == savedHash &&
                          message->text().contains("上書きしません"),
                      "Word UI keeps existing destination");
                range->setFocus();
                range->setText("1-2");
                QTest::keyClick(range, Qt::Key_Return);
                check(QTest::qWaitFor(
                          [&] {
                              return save->isEnabled() &&
                                     dialog->findChild<QComboBox*>("wordTextPage")->count() == 2;
                          },
                          30000),
                      "Word page range Enter updates async draft");
                auto page = dialog->findChild<QComboBox*>("wordTextPage");
                page->setFocus();
                QTest::keyClick(page, Qt::Key_End);
                QStringList expectedSecondPage;
                for (const auto& line : criterion["expected_pages"].toArray()[1].toArray())
                    expectedSecondPage << line.toString();
                // The design excludes annotations from Word body extraction.
                // The signature remains an editable annotation in the original.
                check(editor->toPlainText() == expectedSecondPage.join('\n') &&
                          signatures(window.doc.pdf(), 1).size() == 1,
                      "Exact second Word body; signature remains in original PDF");
                QTest::keyClick(page, Qt::Key_Home);
                check(editor->toPlainText().contains("00123"),
                      "Re-extracted page matches original text");
                auto preview = dynamic_cast<PageRegionPreview*>(
                    dialog->findChild<QWidget*>("wordTextPreview"));
                check(preview && QTest::qWaitFor([&] { return !preview->image.isNull(); }, 15000),
                      "Actual original PDF preview ready before screenshot");
                dialog->grab().save(output + "/word-text-ui.png");
                operated = true;
                QTest::mouseClick(dialog->findChild<QPushButton*>("wordTextClose"), Qt::LeftButton);
            }
            catch (const std::exception& exception)
            {
                error = QString::fromUtf8(exception.what());
                dialog->done(QDialog::Rejected);
            }
        });
    automation.start(25);
    auto action = window.findChild<QAction*>("extractPdfWordText");
    check(action && action->isEnabled(), "Real Word menu route available");
    action->trigger();
    check(error.isEmpty() && operated, "Word UI sequence: " + error);
    check(encodePdf(window.doc.pdf()) == original && window.doc.cursor == cursor &&
              window.doc.dirty() && fileHash(window.doc.source) == sourceHash,
          "Real Word menu/export preserves original PDF, signature and Undo");
    PdfTextDocxDialog cancelled(window.doc.pdf(), 0, {}, &window);
    cancelled.show();
    cancelled.reject();
    check(QTest::qWaitFor([&] { return !cancelled.isVisible(); }, 15000),
          "Owned Word preview cancellation");
    Window restricted;
    restricted.openFile(fixtures + "/pdf-text-docx/" + criterion["copy_restricted"].toString());
    check(!restricted.findChild<QAction*>("extractPdfWordText")->isEnabled(),
          "Copy-restricted Word route disabled");
    return {{"actual_menu_preview_keyboard_edit_save", true},
            {"two_page_preview_switch", true},
            {"existing_destination_retained", true},
            {"PDF_signature_Undo_original_retained", true},
            {"copy_restricted_route_disabled", true},
            {"native_IME_OS_display", "未実行"}};
}
QJsonObject testWordTextOffice(const QString& fixtures, const QString& output)
{
    const auto criterion = fixed(fixtures);
    auto document = readPdf(fixtures + "/pdf-text-docx/" + criterion["source"].toString());
    const auto texts = extractWordText(document, {0, 1});
    const auto input = output + "/word-engine.docx";
    exportWordText(texts, input, "Meiryo UI");
    const auto original = fileHash(input);
    auto imported = importDocx(input, officeConverterPath());
    const auto pdf = output + "/word-engine.pdf";
    writeCandidate(imported, pdf);
    check(imported.getCatalog()->getPageCount() == 2 && fileHash(input) == original,
          "Actual Office engine opens editable Word without modifying input");
    QJsonArray copied;
    for (int page = 0; page < 2; ++page)
        copied.append(pageText(imported, page));
    QFile observed(output + "/word-engine-observed.json");
    check(observed.open(QIODevice::WriteOnly), "Preserve actual Word engine text");
    observed.write(QJsonDocument(copied).toJson());
    observed.close();
    Window viewer;
    viewer.show();
    viewer.openFile(pdf);
    for (int page = 0; page < 2; ++page)
    {
        QStringList phrases;
        for (const auto& value : criterion["expected_pages"].toArray()[page].toArray())
        {
            const auto phrase = value.toString();
            check(pageText(imported, page).contains(phrase), "Exact Word engine phrase: " + phrase);
            phrases << phrase;
        }
        verifyOfficeSearchCopy(viewer, page, phrases);
    }
    return {{"real_engine_load_and_two_pages", true},
            {"all_fixed_text_search_copy", true},
            {"original_DOCX_unchanged", true},
            {"Word_native_GUI", "未実行"}};
}
} // namespace tatsu
