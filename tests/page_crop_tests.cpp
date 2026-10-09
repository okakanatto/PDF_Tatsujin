#include "page_crop_tests.h"
#include "annotation_operations.h"
#include "form_fields.h"
#include "page_crop_dialog.h"
#include "pdfdocumentbuilder.h"
#include "window.h"
#include <QtTest/QTest>
#include <limits>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
QRectF expectedBox(const PDFPage* page, QMarginsF margins)
{
    const double factor = 72.0 / 25.4 / page->getUserUnit();
    const double l = margins.left() * factor, t = margins.top() * factor,
                 r = margins.right() * factor, b = margins.bottom() * factor;
    const auto box = page->getCropBox();
    switch (page->getPageRotation())
    {
    case PageRotation::None:
        return box.adjusted(l, b, -r, -t);
    case PageRotation::Rotate90:
        return box.adjusted(t, l, -b, -r);
    case PageRotation::Rotate180:
        return box.adjusted(r, t, -l, -b);
    case PageRotation::Rotate270:
        return box.adjusted(b, r, -t, -l);
    }
    fail("Unexpected rotation");
}
double boxError(QRectF a, QRectF b)
{
    return qMax(QLineF(a.topLeft(), b.topLeft()).length(),
                QLineF(a.bottomRight(), b.bottomRight()).length());
}
PDFObject pageEntry(const PDFDocument& document, int page, const char* key)
{
    const auto object =
        document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
    return document.getObject(object.getDictionary()->get(key));
}
void retained(const PDFDocument& before, const PDFDocument& after)
{
    for (int i = 0; i < int(before.getCatalog()->getPageCount()); ++i)
    {
        const auto first = before.getCatalog()->getPage(i), second = after.getCatalog()->getPage(i);
        check(first->getMediaBox() == second->getMediaBox() &&
                  first->getUserUnit() == second->getUserUnit() &&
                  first->getPageRotation() == second->getPageRotation(),
              "Physical page dictionaries preserved");
        for (const char* key : {"Contents", "Resources", "Annots"})
            check(pageEntry(before, i, key) == pageEntry(after, i, key),
                  QString("Page %1 %2 preserved").arg(i + 1).arg(key));
        auto old = signatures(before, i), current = signatures(after, i);
        check(old.size() == current.size(), "Annotation count preserved");
        for (int j = 0; j < old.size(); ++j)
            check(old[j].rect == current[j].rect && old[j].text == current[j].text &&
                      old[j].ref == current[j].ref,
                  "Signature coordinates and metadata preserved");
    }
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
    fail("Invalid page crop succeeded");
}
} // namespace
QJsonObject testPageCropGeometry(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    const auto originalHash = fileHash(document.source);
    for (int i = 0; i < 4; ++i)
        document.putSignature(i, "余白調整の署名",
                              document.pdf().getCatalog()->getPage(i)->getCropBox().center(), 12,
                              Qt::black);
    putAnnotation(document, 0, OverlayKind::Rectangle, {130, 140, 50, 30}, "残す注釈", Qt::blue, 1);
    const auto before = document.pdf();
    const int cursor = document.cursor;
    writeCandidate(before, output + "/crop-before.pdf");
    const QMarginsF margins(4, 1, 2, 3);
    auto cropped = cropPages(before, {0, 1, 2, 3}, margins);
    retained(before, cropped);
    double error = 0;
    for (int i = 0; i < 4; ++i)
        error = qMax(error, boxError(cropped.getCatalog()->getPage(i)->getCropBox(),
                                     expectedBox(before.getCatalog()->getPage(i), margins)));
    check(error <= .000001, "Independent rotated physical margins within 0.000001pt");
    auto partial = cropPages(before, {0, 2}, margins);
    check(partial.getCatalog()->getPage(1)->getCropBox() ==
                  before.getCatalog()->getPage(1)->getCropBox() &&
              partial.getCatalog()->getPage(3)->getCropBox() ==
                  before.getCatalog()->getPage(3)->getCropBox(),
          "Unselected pages unchanged");
    check(cropPages(before, {0, 1, 2, 3}, {}) == before, "Zero margins return unchanged document");
    document.commit(cropped);
    check(document.cursor == cursor + 1 && document.dirty(), "All crops in one Undo unit");
    document.undo();
    check(document.pdf() == before, "One Undo restores all pages");
    document.redo();
    check(document.pdf() == cropped, "Redo restores crop");
    document.save(output + "/crop-after.pdf");
    check(fileHash(document.source) == originalHash, "Original input preserved");
    Document reopened;
    reopened.open(output + "/crop-after.pdf");
    auto signature = signatures(reopened.pdf(), 3).front();
    check(signature.text == "余白調整の署名", "Japanese signature remains editable");
    reopened.moveSignature(3, signature, {1, 2});
    check(signatures(reopened.pdf(), 3).front().rect.topLeft() ==
              signature.rect.topLeft() + QPointF(1, 2),
          "Saved signature moves on cropped page");
    reopened.undo();
    check(signatures(reopened.pdf(), 3).front().rect == signature.rect, "Reedited signature Undo");
    Document form;
    form.open(fixtures + "/D07.pdf");
    auto fields = formFields(form.pdf());
    auto field = std::find_if(fields.begin(), fields.end(),
                              [](const FormField& value) { return value.name == "name"; });
    check(field != fields.end(), "Find synthetic form");
    putFormValue(form, field->widget, {"髙橋 香織"});
    form.putSignature(0, "入力と署名を保持", {100, 100}, 12, Qt::black);
    writeCandidate(form.pdf(), output + "/crop-form-before.pdf");
    const auto oldForm = form.pdf();
    form.commit(cropPages(form.pdf(), {0}, {1, 1, 1, 1}));
    retained(oldForm, form.pdf());
    form.save(output + "/crop-form.pdf");
    Document formReopened;
    formReopened.open(output + "/crop-form.pdf");
    const auto values = formFields(formReopened.pdf());
    check(std::any_of(values.begin(), values.end(), [](const FormField& value)
                      { return value.name == "name" && value.values == QStringList{"髙橋 香織"}; }),
          "Saved cropped Japanese form value retained");
    return {{"pages", 4},
            {"max_error_pt", error},
            {"one_Undo", true},
            {"saved_signature_reedited", true},
            {"Japanese_form_retained", true},
            {"unselected_unchanged", true}};
}
QJsonObject testPageCropFailures(const QString& fixtures)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    const auto before = encodePdf(source);
    QJsonArray rejected;
    auto invalid = [&](const QString& name, const std::function<void()>& operation)
    {
        rejected.append(QJsonObject{{"case", name}, {"error", rejects(operation)}});
        check(encodePdf(source) == before, "Invalid crop preserves full original");
    };
    for (const QVector<int>& pages :
         {QVector<int>{}, QVector<int>{-1}, QVector<int>{4}, QVector<int>{0, 0}})
        invalid("invalid range", [&] { cropPages(source, pages, {1, 1, 1, 1}); });
    for (double value : {-1., 501., std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()})
        invalid("invalid margin", [&] { cropPages(source, {0, 1}, {value, 1, 1, 1}); });
    invalid("too little page remains", [&] { cropPages(source, {0, 1}, {400, 1, 400, 1}); });
    invalid("empty document", [&] { cropPages(PDFDocument{}, {0}, {}); });
    invalid("missing page", [&] { croppedPageBox(nullptr, {}); });
    auto protectedPdf = readPdf(fixtures + "/viewer-navigation-restricted.pdf", "navigation-user");
    invalid("protected PDF", [&] { cropPages(protectedPdf, {0}, {1, 1, 1, 1}); });
    PDFDocumentBuilder builder(&source);
    builder.setPageCropBox(source.getCatalog()->getPage(3)->getPageReference(), {35, 35, 1, 1});
    const auto tiny = builder.build();
    const auto tinyBefore = encodePdf(tiny);
    invalid("invalid later page", [&] { cropPages(tiny, {0, 3}, {1, 1, 1, 1}); });
    check(encodePdf(tiny) == tinyBefore, "Later failure never partially edits earlier page");
    return {{"rejected", rejected}, {"all_originals_retained", true}};
}
QJsonObject testPageCropUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(800, 480);
    window.show();
    window.openFile(fixtures + "/D02.pdf");
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 10000),
          "Initial PDF visible");
    window.findChild<QAction*>("organizeAction")->trigger();
    auto organizer = static_cast<PageOrganizer*>(window.findChild<QWidget*>("pageOrganizer"));
    organizer->previews->clearSelection();
    organizer->previews->item(1)->setSelected(true);
    organizer->previews->item(3)->setSelected(true);
    auto button = window.findChild<QPushButton*>("cropOrganizerPages");
    check(button && button->isEnabled(), "Crop reachable from organizer");
    for (auto ancestor = button->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        if (auto scroll = qobject_cast<QScrollArea*>(ancestor))
            scroll->ensureWidgetVisible(button);
    const auto before = window.doc.pdf();
    const int cursor = window.doc.cursor;
    int state = 0, ticks = 0, busyTicks = 0;
    QString error;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer automation;
    QObject::connect(
        &automation, &QTimer::timeout,
        [&]
        {
            ++ticks;
            if (auto message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            {
                error = message->text();
                message->accept();
                return;
            }
            auto dialog = dynamic_cast<PageCropDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            for (auto thread : dialog->findChildren<QThread*>())
                if (thread->isRunning())
                    ++busyTicks;
            if (elapsed.elapsed() > 30000)
            {
                error = "Crop UI deadline";
                dialog->reject();
                return;
            }
            try
            {
                auto apply = dialog->findChild<QPushButton*>("applyPageCrop");
                if (state == 0)
                {
                    dialog->resize(600, 450);
                    check(!apply->isEnabled(), "Zero input cannot add an Undo entry");
                    dialog->findChild<QDoubleSpinBox*>("cropLeft")->setValue(4);
                    dialog->findChild<QDoubleSpinBox*>("cropTop")->setValue(1);
                    dialog->findChild<QDoubleSpinBox*>("cropRight")->setValue(2);
                    dialog->findChild<QDoubleSpinBox*>("cropBottom")->setValue(3);
                    auto page = dialog->findChild<QComboBox*>("cropPreviewPage");
                    page->setCurrentIndex(1);
                    page->setCurrentIndex(0);
                    page->setCurrentIndex(1);
                    state = 1;
                }
                else if (state == 1 && apply->isEnabled())
                {
                    auto left = dialog->findChild<QDoubleSpinBox*>("cropLeft");
                    left->setValue(500);
                    check(!apply->isEnabled(), "Invalid all-page margin is blocked inline");
                    left->setValue(4);
                    check(apply->isEnabled(), "Valid input recovers without rebuilding document");
                    check(ticks > 2 && busyTicks > 0,
                          "GUI events handled while preview worker runs");
                    dialog->grab().save(output + "/m4-page-crop-dialog.png");
                    state = 2;
                    QTest::mouseClick(apply, Qt::LeftButton);
                }
            }
            catch (const std::exception& failure)
            {
                error = QString::fromUtf8(failure.what());
                dialog->reject();
            }
        });
    automation.start(1);
    QTest::mouseClick(button, Qt::LeftButton);
    automation.stop();
    check(error.isEmpty(), error);
    check(state == 2 && window.doc.cursor == cursor + 1, "Actual crop applies as one operation");
    const auto cropped = window.doc.pdf();
    const QMarginsF margins(4, 1, 2, 3);
    for (int i = 0; i < 4; ++i)
        check(boxError(cropped.getCatalog()->getPage(i)->getCropBox(),
                       i == 1 || i == 3 ? expectedBox(before.getCatalog()->getPage(i), margins)
                                        : before.getCatalog()->getPage(i)->getCropBox()) <= .000001,
              "Actual selected-page result");
    window.undoAction->trigger();
    check(window.doc.pdf() == before, "Product Undo restores crop");
    window.redoAction->trigger();
    check(window.doc.pdf() == cropped, "Product Redo restores crop");
    window.doc.save(output + "/m4-page-crop-ui.pdf");
    int cancelled = 0;
    QTimer cancel;
    QObject::connect(&cancel, &QTimer::timeout,
                     [&]
                     {
                         if (auto dialog =
                                 dynamic_cast<PageCropDialog*>(QApplication::activeModalWidget()))
                         {
                             if (dialog->findChild<QPushButton*>("applyPageCrop")->isEnabled())
                                 error = "Zero input unexpectedly enabled";
                             ++cancelled;
                             dialog->reject();
                         }
                     });
    cancel.start(10);
    const int savedCursor = window.doc.cursor;
    QTest::mouseClick(button, Qt::LeftButton);
    cancel.stop();
    check(error.isEmpty() && cancelled == 1 && window.doc.cursor == savedCursor &&
              window.doc.pdf() == cropped && !window.doc.dirty(),
          "Cancel and zero input preserve saved PDF and Undo");
    return {{"selected_pages", QJsonArray{2, 4}},
            {"event_loop_ticks", ticks},
            {"preview_busy_event_loop_ticks", busyTicks},
            {"one_Undo", true},
            {"cancel_zero_preserved", true},
            {"native_UI", "未実行"}};
}
} // namespace tatsu
