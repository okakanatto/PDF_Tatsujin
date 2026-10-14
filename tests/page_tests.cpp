#include "page_tests.h"
#include "pdfform.h"
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
int fieldCount(const PDFDocument& document)
{
    auto form = PDFForm::parse(&document, document.getCatalog()->getFormObject());
    int count = 0;
    form.apply(
        [&](const PDFFormField* field)
        {
            if (!field->getWidgets().empty())
                ++count;
        });
    return count;
}
} // namespace
QJsonObject testPageArrange(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    const auto sourceHash = fileHash(document.source);
    QVector<QImage> images;
    for (int page = 0; page < document.pages(); ++page)
    {
        auto point =
            document.pdf().getCatalog()->getPage(page)->getCropBox().topLeft() + QPointF(20, 30);
        document.putSignature(page, QString("ページ%1の署名").arg(page + 1), point, 10, Qt::black);
        images.append(renderPage(document.pdf(), page, 1));
    }
    const auto before = document.pdf();
    const QVector<int> order{3, 1, 0, 2};
    document.commit(selectPages(document.pdf(), order));
    for (int page = 0; page < order.size(); ++page)
    {
        check(renderPage(document.pdf(), page, 1) == images[order[page]],
              "page content, annotation, CropBox, rotation and UserUnit follow reordered page");
        check(signatures(document.pdf(), page).at(0).text ==
                  QString("ページ%1の署名").arg(order[page] + 1),
              "signature stays with its page");
    }
    document.undo();
    check(document.pdf() == before, "one structural Undo restores original PDF");
    document.redo();
    document.save(output + "/pages-reordered.pdf");
    check(fileHash(document.source) == sourceHash, "page editing protects original file");
    document.commit(selectPages(document.pdf(), {2}));
    check(document.pages() == 1 && renderPage(document.pdf(), 0, 1) == images[0],
          "page deletion keeps selected original page");
    const auto single = document.pdf();
    bool refused = false;
    try
    {
        document.commit(selectPages(document.pdf(), {}));
    }
    catch (const std::exception&)
    {
        refused = true;
    }
    check(refused && document.pdf() == single, "last page deletion rejected atomically");
    check(movePageOrder(5, {3, 1}, 0) == QVector<int>({1, 3, 0, 2, 4}),
          "multi-selection retains relative order");
    document.save(output + "/pages-deleted.pdf");
    return {{"page_contents_exact", true},
            {"annotation_follow", true},
            {"undo_redo", true},
            {"last_page_protected", true}};
}
QJsonObject testPageMerge(const QString& fixtures, const QString& output)
{
    Document first, second;
    first.open(fixtures + "/D01.pdf");
    second.open(fixtures + "/D07.pdf");
    const auto hash1 = fileHash(first.source), hash2 = fileHash(second.source);
    second.putSignature(0, "フォームに署名", {60, 50}, 16, Qt::black);
    const auto originalFormCount = fieldCount(second.pdf());
    check(originalFormCount >= 6, "standard form fixture present");
    auto merged = mergeDocuments({first.pdf(), second.pdf()});
    check(int(merged.getCatalog()->getPageCount()) == first.pages() + second.pages(),
          "merge page order and count");
    check(pageText(merged, 0) == pageText(first.pdf(), 0), "first document body retained");
    check(renderPage(merged, first.pages(), 1.2) == renderPage(second.pdf(), 0, 1.2),
          "form, annotation, link appearance and signature preserved in merge");
    check(fieldCount(merged) == originalFormCount, "form fields retained");
    writeCandidate(merged, output + "/pages-merged.pdf");
    auto extracted = selectPages(merged, {first.pages()});
    check(fieldCount(extracted) == originalFormCount, "extracted form page remains a form");
    auto bodyOnly = selectPages(merged, {0});
    check(fieldCount(bodyOnly) == 0, "fields belonging only to deleted pages removed");
    writeCandidate(extracted, output + "/pages-form-extracted.pdf");
    first.commit(insertPages(first.pdf(), second.pdf(), {0}, 0));
    check(renderPage(first.pdf(), 0, 1.2) == renderPage(second.pdf(), 0, 1.2),
          "insertion retains form page without flattening");
    const auto candidate = first.pdf();
    bool refused = false;
    QString error;
    try
    {
        first.commit(insertPages(first.pdf(), second.pdf(), {0}, 0));
    }
    catch (const std::exception& e)
    {
        refused = true;
        error = QString::fromUtf8(e.what());
    }
    check(refused && error.contains("フォーム名") && first.pdf() == candidate,
          "form-name collision rejects before any partial insertion");
    check(fileHash(first.source) == hash1 && fileHash(second.source) == hash2,
          "input files unchanged");
    return {{"form_fields", originalFormCount},
            {"merge_insert_extract", true},
            {"collision_atomic", true}};
}
QJsonObject testPageExports(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    const auto original = document.pdf();
    const auto sourceHash = fileHash(document.source);
    const QString blocked = output + "/split-002.pdf";
    QFile existing(blocked);
    check(existing.open(QIODevice::WriteOnly), "existing destination setup");
    existing.write("existing output must survive");
    existing.close();
    const auto existingHash = fileHash(blocked);
    auto results =
        exportPageGroups(document.pdf(), {{0}, {1}, {2}},
                         {output + "/split-001.pdf", blocked, output + "/split-003.pdf"});
    check(results.size() == 3 && results[0].success && !results[1].success && results[2].success,
          "partial failures reported individually and later outputs still attempted");
    check(!results[1].error.isEmpty() && fileHash(blocked) == existingHash,
          "existing output never overwritten");
    check(document.pdf() == original && !document.dirty() &&
              fileHash(document.source) == sourceHash,
          "extract/split never edits original");
    auto exported = readPdf(results[2].path);
    check(renderPage(exported, 0, 1) == renderPage(document.pdf(), 2, 1),
          "split page content and rotation match source");
    bool refused = false;
    try
    {
        exportPageGroups(document.pdf(), {{0}, {0}},
                         {output + "/duplicate-1.pdf", output + "/duplicate-2.pdf"});
    }
    catch (const std::exception&)
    {
        refused = true;
    }
    check(refused && !QFile::exists(output + "/duplicate-1.pdf"),
          "overlapping ranges rejected before output");
    return {{"success_count", 2},
            {"failure_count", 1},
            {"existing_and_source_preserved", true},
            {"overlap_rejected", true}};
}
QJsonObject testPageOrganizer(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    QTest::qWait(100);
    window.canvas->setZoom(1.4);
    window.canvas->navigatePage(2);
    QTest::qWait(80);
    const auto old = window.doc.pdf();
    window.findChild<QAction*>("organizeAction")->trigger();
    auto organizer = static_cast<PageOrganizer*>(window.findChild<QWidget*>("pageOrganizer"));
    check(organizer->isVisible(), "central page organizer visible");
    check(QTest::qWaitFor([&] { return !organizer->previews->preview(2).isNull(); }, 10000),
          "actual page preview rendered");
    organizer->previews->clearSelection();
    organizer->previews->item(1)->setSelected(true);
    organizer->previews->item(3)->setSelected(true);
    auto settings = window.findChild<QWidget*>("organizerSettings");
    settings->findChild<QSpinBox*>()->setValue(1);
    QPushButton* move = nullptr;
    for (auto button : settings->findChildren<QPushButton*>())
        if (button->text() == "指定位置へ移動")
            move = button;
    check(move, "actual move control exists");
    move->click();
    check(window.doc.cursor == 1 && window.doc.pages() == 4, "multi-page move is one commit");
    check(window.doc.pdf().getCatalog()->getPage(0)->getPageRotation() ==
              old.getCatalog()->getPage(1)->getPageRotation(),
          "UI moves intended first selected page");
    window.undoAction->trigger();
    check(window.doc.pdf() == old, "UI Undo restores all page metadata");
    window.redoAction->trigger();
    window.doc.save(output + "/pages-organizer-ui.pdf");
    check(QTest::qWaitFor([&] { return !organizer->previews->preview(0).isNull(); }, 10000),
          "changed page grid preview ready");
    check(QTest::qWaitFor(
              [&]
              {
                  for (int page = 0; page < window.doc.pages(); ++page)
                      if (organizer->previews->preview(page).isNull())
                          return false;
                  return true;
              },
              10000),
          "all visible organizer previews ready");
    window.grab().save(output + "/pages-organizer-ui.png");
    organizer->findChild<QPushButton*>("returnFromOrganizer")->click();
    QTest::qWait(100);
    check(window.canvas->isVisible() && !organizer->isVisible(), "return to real PDF viewer");
    check(window.canvas->page == 3 && qAbs(window.canvas->zoom - 1.4) < .001,
          QString("previous reading page follows its move and zoom retained (page=%1, zoom=%2)")
              .arg(window.canvas->page)
              .arg(window.canvas->zoom));
    return {{"central_grid", true},
            {"multi_select_move", true},
            {"undo_redo", true},
            {"reading_position_return", true}};
}
} // namespace tatsu
