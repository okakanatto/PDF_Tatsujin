#include "office_import.h"
#include "office_import_dialog.h"
#include "office_import_tests.h"
#include "office_pdf_compat.h"
#include "window.h"
#include <QtTest/QTest>

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
    const auto path = fixtures + "/office-import/sheets-slides/criteria.json";
    check(fileHash(path).toHex() ==
              "71d849f6df2d8743e291fe2038ddfc819881588eef226b11070d74b51f4addbc",
          "Frozen spreadsheet/presentation criteria SHA");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read spreadsheet/presentation criteria");
    return QJsonDocument::fromJson(file.readAll()).object();
}
} // namespace
QJsonObject testOfficeSheetsSlides(const QString& fixtures, const QString& output)
{
    check(splitOfficeClosingAdjustments("[(a)102(b)-120(c)12.5]TJ") ==
              "[(a)25 25 25 25 2(b)-120(c)12.5]TJ",
          "Exact summed positive adjustment only");
    for (const auto& unchanged : {QByteArray("[(102)102]foo"), QByteArray("[(102)-102]TJ"),
                                  QByteArray("BI /W 1 ID 102 EI"), QByteArray("[[(a)102]]TJ")})
        check(splitOfficeClosingAdjustments(unchanged) == unchanged,
              "Non-text, negative, inline-image and nested operands untouched");
    const auto fixed = criteria(fixtures);
    const auto hashes = fixed["files"].toObject();
    for (auto i = hashes.begin(); i != hashes.end(); ++i)
        check(fileHash(fixtures + "/office-import/sheets-slides/" + i.key()).toHex() ==
                  i.value().toString().toLatin1(),
              "Frozen Office input SHA");
    for (const auto& name : {QString("sheets.xlsx"), QString("slides.pptx")})
    {
        const auto path = fixtures + "/office-import/sheets-slides/" + name;
        const auto before = fileHash(path);
        auto candidate = importOffice(path, officeConverterPath());
        writeCandidate(candidate,
                       output + "/office-" + QFileInfo(name).completeBaseName() + ".pdf");
        const auto pages = fixed["expected_pages"].toObject()[name].toArray();
        check(candidate.getCatalog()->getPageCount() == pages.size(), "Office frozen page count");
        for (int i = 0; i < pages.size(); ++i)
        {
            const auto text = pageText(candidate, i);
            QFile diagnostic(output + QString("/office-%1-%2.txt").arg(name).arg(i));
            check(diagnostic.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Text diagnostic");
            diagnostic.write(text.toUtf8());
            const auto dimensions = pageSize(candidate.getCatalog()->getPage(i));
            const auto size = fixed["page_size_pt"].toObject()[name].toArray();
            check(qAbs(dimensions.width() - size[0].toDouble()) <= .5 &&
                      qAbs(dimensions.height() - size[1].toDouble()) <= .5,
                  "Office page geometry");
            for (const auto& expected : pages[i].toArray())
                check(text.contains(expected.toString()),
                      "Office searchable text: " + expected.toString());
            check(!renderPage(candidate, i, 1).isNull(), "Office PDF actual render");
        }
        Document document;
        document.history = {candidate};
        document.saved = -1;
        const auto initial = encodePdf(document.pdf());
        const auto signature = document.putSignature(0, "日本語の署名", {100, 250}, 16, Qt::black);
        document.moveSignature(0, signature, {20, 15});
        document.undo();
        document.undo();
        check(encodePdf(document.pdf()) == initial, "Office signature Undo restores PDF");
        document.redo();
        document.redo();
        document.save(output + "/office-" + QFileInfo(name).completeBaseName() + "-signed.pdf");
        Document reopened;
        reopened.open(document.target);
        const auto saved = signatures(reopened.pdf(), 0);
        check(saved.size() == 1 && saved[0].text == "日本語の署名",
              "Saved Office editable signature");
        reopened.moveSignature(0, saved[0], {5, 5});
        reopened.save(output + "/office-" + QFileInfo(name).completeBaseName() + "-reedited.pdf");
        check(fileHash(path) == before, "Office original unchanged");
    }
    int refused = 0;
    for (const auto& name : fixed["refused"].toArray())
    {
        try
        {
            validatedOffice(fixtures + "/office-import/sheets-slides/" + name.toString());
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    }
    check(refused == 4, "Macros, embeddings, external books and formulas refused");
    return {{"XLSX_PPTX_pages_text_geometry_render", true},
            {"signature_history_save_reedit", true},
            {"refused", refused}};
}
QJsonObject testOfficeSheetsSlidesUi(const QString& fixtures, const QString& output)
{
    for (const auto& name : {QString("sheets.xlsx"), QString("slides.pptx")})
    {
        OfficeImportDialog dialog(fixtures + "/office-import/sheets-slides/" + name);
        dialog.show();
        check(!dialog.findChild<QCheckBox*>("officeImportSuppressSpacing")->isVisible(),
              "Word-only spacing control not offered for other components");
        auto apply = dialog.findChild<QPushButton*>("officeImportApply");
        check(QTest::qWaitFor([&] { return apply->isEnabled(); }, 120000),
              "Office component preview ready");
        auto page = dialog.findChild<QSpinBox*>("officeImportPage");
        page->setFocus();
        QTest::keyClick(page, Qt::Key_Up);
        check(page->value() == 2, "Office second page navigation");
        check(QTest::qWaitFor([&] { return apply->isEnabled(); }, 15000),
              "Office second page render");
        check(dialog.grab().save(output + "/office-" + QFileInfo(name).completeBaseName() +
                                 "-ui.png"),
              "Actual Office UI screenshot");
        QTest::mouseClick(apply, Qt::LeftButton);
        check(dialog.result() == QDialog::Accepted, "Office explicit application");
        auto candidate = dialog.takeDocument();
        writeCandidate(candidate,
                       output + "/office-" + QFileInfo(name).completeBaseName() + "-ui.pdf");
        Window window;
        window.doc.history = {std::move(candidate)};
        window.doc.saved = -1;
        ++window.doc.revision;
        window.refresh(true);
        window.show();
        verifyOfficeSearchCopy(window, 0,
                               name == "sheets.xlsx" ? QStringList{"日本語の表"}
                                                     : QStringList{"検索とコピーを確認します。"});
        verifyOfficeSearchCopy(window, 1,
                               name == "sheets.xlsx" ? QStringList{"English worksheet"}
                                                     : QStringList{"English presentation"});
        check(window.grab().save(output + "/office-" + QFileInfo(name).completeBaseName() +
                                 "-search-copy.png"),
              "Office viewer search/copy screenshot");
    }
    return {{"actual_XLSX_PPTX_preview_page_apply", true},
            {"actual_Japanese_English_search_selection_copy", true},
            {"native_Office_GUI", "未実行"}};
}
} // namespace tatsu
