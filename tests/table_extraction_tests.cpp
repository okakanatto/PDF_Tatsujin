#include "table_extraction_tests.h"
#include "office_import.h"
#include "table_extraction.h"
#include "table_extraction_dialog.h"
#include <QtCore/private/qzipreader_p.h>
#include <QtCore>
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
} // namespace
QJsonObject testTableExtractionUi(const QString& fixtures, const QString& output)
{
    auto document = readPdf(fixtures + "/table-extraction/table.pdf");
    const auto original = encodePdf(document);
    TableExtractionDialog dialog(document, 0, [] {});
    dialog.show();
    auto preview = dynamic_cast<TableGridPreview*>(dialog.findChild<QWidget*>("tableGridPreview"));
    check(preview, "Actual table preview widget");
    auto save = dialog.findChild<QPushButton*>("tableSave");
    auto cells = dialog.findChild<QTableWidget*>("tableCells");
    check(QTest::qWaitFor([&] { return !preview->image.isNull(); }, 15000),
          "Actual table page render");
    auto draw = dialog.findChild<QPushButton*>("tableDraw");
    preview->setFocus();
    QTest::keyClick(preview, Qt::Key_Escape);
    check(!preview->drawing && !draw->isChecked(), "Escape cancels range drawing coherently");
    QTest::mouseClick(draw, Qt::LeftButton);
    check(preview->drawing && draw->isChecked(), "Drawing restarts with one click");
    const auto a = preview->physicalToWidget({40, 70}).toPoint();
    const auto b = preview->physicalToWidget({340, 210}).toPoint();
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, a);
    QTest::mouseMove(preview, b, 20);
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, b);
    check(QTest::qWaitFor([&] { return save->isEnabled(); }, 15000), "Actual region extraction");
    auto boundary = dialog.findChild<QDoubleSpinBox*>("tableBoundaryPosition");
    boundary->setFocus();
    QTest::keyClick(boundary, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(boundary, "35");
    QTest::keyClick(boundary, Qt::Key_Return);
    check(QTest::qWaitFor([&] { return save->isEnabled(); }, 15000),
          "Keyboard boundary adjustment");
    const auto before = preview->grid.columns[1];
    const auto start = preview->physicalToWidget({40 + before * 300, 100}).toPoint();
    const auto finish = start + QPoint(5, 0);
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(preview, finish, 20);
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, finish);
    if (preview->grid.columns[1] == before)
    {
        dialog.grab().save(output + "/table-drag-failure.png");
        const auto region = preview->grid.region;
        fail(QString("Actual drag boundary change; drawing=%1 region=%2,%3,%4,%5 start=%6,%7 "
                     "finish=%8,%9 paper=%10,%11,%12,%13")
                 .arg(preview->drawing)
                 .arg(region.x())
                 .arg(region.y())
                 .arg(region.width())
                 .arg(region.height())
                 .arg(start.x())
                 .arg(start.y())
                 .arg(finish.x())
                 .arg(finish.y())
                 .arg(preview->paper().x())
                 .arg(preview->paper().y())
                 .arg(preview->paper().width())
                 .arg(preview->paper().height()));
    }
    auto message = dialog.findChild<QLabel*>("tableMessage");
    check(QTest::qWaitFor([&] { return message->text().contains("セル境界に文字"); }, 15000),
          "Actual character-crossing boundary refuses export");
    check(!save->isEnabled(), "Crossing boundary cannot save partial cells");
    const auto region = preview->grid.region;
    const auto correctionStart =
        preview
            ->physicalToWidget(
                {region.left() + preview->grid.columns[1] * region.width(), region.top() + 30})
            .toPoint();
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, correctionStart);
    QTest::mouseMove(preview, correctionStart - QPoint(10, 0), 20);
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, correctionStart - QPoint(10, 0));
    check(QTest::qWaitFor([&] { return save->isEnabled(); }, 15000),
          "Corrected drag extraction ready");
    cells->setCurrentCell(1, 1);
    cells->setFocus();
    QTest::keyClick(cells, Qt::Key_F2);
    auto editor = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    check(editor && cells->isAncestorOf(editor), "Actual table cell editor focused");
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(editor, "0099");
    QTest::keyClick(editor, Qt::Key_Return);
    check(QTest::qWaitFor([&] { return cells->item(1, 1)->text() == "0099"; }, 15000),
          "Actual cell keyboard edit committed");
    bool keptEdit = false;
    QTimer::singleShot(0,
                       [&]
                       {
                           if (auto question =
                                   qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                           {
                               keptEdit = true;
                               QTest::mouseClick(question->button(QMessageBox::No), Qt::LeftButton);
                           }
                       });
    auto rows = dialog.findChild<QSpinBox*>("tableRows");
    rows->setFocus();
    QTest::keyClick(rows, Qt::Key_Up);
    check(keptEdit && rows->value() == 4 && cells->item(1, 1)->text() == "0099",
          "Declining re-extraction retains edited cells and grid");
    const auto path = output + "/table-ui.xlsx";
    dialog.findChild<QLineEdit*>("tableOutputPath")->setText(path);
    QTest::mouseClick(save, Qt::LeftButton);
    check(QFileInfo::exists(path), "Actual new XLSX save");
    check(dialog.grab().save(output + "/table-extraction-ui.png"), "Actual table screenshot");
    QTest::mouseClick(dialog.findChild<QPushButton*>("tableCancel"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return !dialog.isVisible(); }, 15000), "Table dialog close");
    check(encodePdf(document) == original, "Table UI preserves original PDF");
    return {{"actual_region_boundary_keyboard_drag_cell_edit_save_close", true},
            {"native_IME", "未実行"}};
}
QJsonObject testTableExtraction(const QString& fixtures, const QString& output)
{
    const auto root = fixtures + "/table-extraction/";
    check(fileHash(root + "criteria.json").toHex() ==
              "07c15ed09f71d8b5f1b5eeff45b65a00a313d8a05492f43346e7d9718b752672",
          "Frozen table criteria SHA");
    QFile file(root + "criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read fixed table criteria");
    const auto fixed = QJsonDocument::fromJson(file.readAll()).object();
    const auto hashes = fixed["files"].toObject();
    for (auto i = hashes.begin(); i != hashes.end(); ++i)
        check(fileHash(root + i.key()).toHex() == i.value().toString().toLatin1(),
              "Fixed table input SHA");
    TableCells expected;
    for (const auto& row : fixed["expected_cells"].toArray())
    {
        QStringList values;
        for (const auto& cell : row.toArray())
            values << cell.toString();
        expected << values;
    }
    for (const auto& value : fixed["cases"].toArray())
    {
        const auto c = value.toObject();
        const auto name = c["file"].toString();
        const auto region = c["region_pt"].toArray();
        TableGrid grid{{region[0].toDouble(), region[1].toDouble(), region[2].toDouble(),
                        region[3].toDouble()},
                       {0, .25, .5, .75, 1},
                       {0, 1.0 / 3, 2.0 / 3, 1}};
        auto document = readPdf(root + name);
        const auto original = encodePdf(document);
        const auto cells = extractTableCells(document, 0, grid);
        QJsonArray diagnostic;
        for (const auto& row : cells)
            diagnostic.append(QJsonArray::fromStringList(row));
        QFile result(output + "/table-" + QFileInfo(name).completeBaseName() + ".json");
        check(result.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Fresh table diagnostic");
        result.write(QJsonDocument(diagnostic).toJson());
        QJsonArray expectedDiagnostic;
        for (const auto& row : expected)
            expectedDiagnostic.append(QJsonArray::fromStringList(row));
        check(cells == expected,
              "Exact table cells; expected=" +
                  QString::fromUtf8(
                      QJsonDocument(expectedDiagnostic).toJson(QJsonDocument::Compact)) +
                  "; actual=" +
                  QString::fromUtf8(QJsonDocument(diagnostic).toJson(QJsonDocument::Compact)));
        const auto path = output + "/table-" + QFileInfo(name).completeBaseName() + ".xlsx";
        exportTableXlsx(cells, path);
        const auto saved = fileHash(path);
        bool refused = false;
        try
        {
            exportTableXlsx(cells, path);
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        check(refused && fileHash(path) == saved, "Existing workbook retained");
        check(encodePdf(document) == original, "Table extraction leaves source PDF unchanged");
    }
    int refused = 0;
    auto document = readPdf(root + "table.pdf");
    TableGrid grid{{40, 70, 300, 140}, {0, .25, .5, .75, 1}, {0, 1.0 / 3, 2.0 / 3, 1}};
    for (int condition = 0; condition < 4; ++condition)
        try
        {
            if (condition == 0)
                extractTableCells(readPdf(root + "image-only.pdf"), 0, grid);
            else if (condition == 1)
                extractTableCells(document, 0,
                                  {{40, 70, 300, 140}, {0, .25, .5, .75, 1}, {0, .04, .7, 1}});
            else if (condition == 2)
                extractTableCells(document, 0, {{40, 70, 300, 140}, {0, .5, .25, 1}, {0, .5, 1}});
            else
                exportTableXlsx(expected, output + "/table-cancelled.xlsx", [] { return true; });
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    check(refused == 4 && !QFileInfo::exists(output + "/table-cancelled.xlsx"),
          "Table refusals and cancellation");
    bool copyRefused = false;
    try
    {
        extractTableCells(readPdf(fixtures + "/viewer-selection-restricted.pdf", "selection-user"),
                          0, grid);
    }
    catch (const std::exception&)
    {
        copyRefused = true;
    }
    check(copyRefused, "Table extraction honors denied copying");
    const auto race = output + "/table-publication-race.xlsx";
    bool collision = false;
    bool competitorCreated = false;
    try
    {
        exportTableXlsx(expected, race,
                        [&]
                        {
                            if (competitorCreated)
                                return false;
                            for (const auto& name :
                                 QDir(output).entryList({"PDFTatsujin-office-*"}, QDir::Dirs))
                                if (QFileInfo::exists(output + '/' + name + "/candidate.xlsx"))
                                {
                                    QZipReader candidate(output + '/' + name + "/candidate.xlsx");
                                    if (candidate.status() != QZipReader::NoError ||
                                        candidate.fileInfoList().size() != 6)
                                        continue;
                                    QFile competitor(race);
                                    check(
                                        competitor.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                                        "Concurrent output writer");
                                    competitor.write("concurrent-workbook-sentinel");
                                    competitor.close();
                                    competitorCreated = true;
                                }
                            return false;
                        });
    }
    catch (const std::exception&)
    {
        collision = true;
    }
    QFile competitor(race);
    check(collision && competitor.open(QIODevice::ReadOnly) &&
              competitor.readAll() == "concurrent-workbook-sentinel",
          "Publication race retains competitor bytes");
    return {{"exact_cells_two_coordinate_cases", true},
            {"copy_denied_and_publication_race_preserved", true},
            {"source_preserved", true},
            {"refused", refused}};
}
QJsonObject testTableOfficeInterop(const QString& output)
{
    const auto source = output + "/table-table.xlsx";
    const auto before = fileHash(source);
    auto converted = importOffice(source, officeConverterPath());
    check(converted.getCatalog()->getPageCount() == 1, "Actual Calc workbook page count");
    const auto text = pageText(converted, 0);
    for (const auto& phrase :
         {QString("品目"), QString("番号"), QString("備考"), QString("項目一"), QString("0012"),
          QString("=1+2"), QString("二行"), QString("改行"), QString("English text"),
          QString("項目二"), QString("30"), QString("合計")})
        check(text.contains(phrase), "Actual Calc workbook literal text: " + phrase);
    writeCandidate(converted, output + "/table-calc.pdf");
    check(!renderPage(converted, 0, 1).isNull(), "Actual Calc PDF rendering");
    check(fileHash(source) == before, "Actual Calc input unchanged");
    return {{"actual_LibreOffice_read_convert_literal_strings_render", true},
            {"native_Excel_GUI", "未実行"}};
}
} // namespace tatsu
