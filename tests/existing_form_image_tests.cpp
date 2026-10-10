#include "existing_image_dialog.h"
#include "existing_image_edit.h"
#include "existing_image_tests.h"
#include "form_fields.h"
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
QRectF bounds(const QJsonArray& a)
{
    return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()};
}
ExistingImage find(const PDFDocument& document, QRectF box)
{
    for (const auto& image : existingImages(document, 0, {}, true))
        if (qAbs(image.physical.x() - box.x()) < 1e-7 &&
            qAbs(image.physical.y() - box.y()) < 1e-7 &&
            qAbs(image.physical.width() - box.width()) < 1e-7 &&
            qAbs(image.physical.height() - box.height()) < 1e-7)
            return image;
    fail("Fixed nested image missing");
}
void retained(PDFDocument before, PDFDocument after)
{
    for (int p = 0; p < int(before.getCatalog()->getPageCount()); ++p)
        check(pageText(before, p) == pageText(after, p), "Nested image edit preserves all text");
    const auto a = formFields(before), b = formFields(after);
    check(a.size() == b.size(), "Nested image edit preserves fields");
    for (int i = 0; i < a.size(); ++i)
        check(a[i].qualifiedName == b[i].qualifiedName && a[i].values == b[i].values,
              "Nested image form values identical");
}
} // namespace
QJsonObject testExistingFormImages(const QString& fixtures, const QString& output)
{
    const auto base = fixtures + "/existing-image-edit/forms/";
    check(fileHash(base + "criteria.json").toHex() ==
              "a060299145df5301d76491f31b5306c2e767608702620a2f4d7b39d2f662d04b",
          "Initial wrong-height proposal retained unchanged");
    QFile file(base + "positive-criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen nested image criteria");
    const auto fixed = QJsonDocument::fromJson(file.readAll()).object();
    const auto hashes = fixed["files"].toObject();
    for (auto it = hashes.begin(); it != hashes.end(); ++it)
        check(fileHash(base + it.key()).toHex() == it.value().toString().toLatin1(),
              "Frozen Form image input SHA");
    const auto path = base + "shared-forms.pdf";
    const auto source = readPdf(path);
    const auto initial = encodePdf(source);
    const auto original = bounds(fixed["first_physical"].toArray()),
               target = bounds(fixed["target_physical"].toArray());
    check(existingImages(source, 0).size() == 2 && existingImages(source, 0, {}, true).size() == 4,
          "Optional nested mode preserves direct enumeration");
    const auto first = find(source, original);
    auto moved = editExistingImage(source, 0, first.occurrence, ExistingImageChange::Geometry,
                                   target, {}, {}, true);
    retained(source, moved);
    find(moved, target);
    writeCandidate(moved, output + "/existing-form-image-moved.pdf");
    auto restored = editExistingImage(moved, 0, find(moved, target).occurrence,
                                      ExistingImageChange::Geometry, original, {}, {}, true);
    retained(source, restored);
    find(restored, original);
    writeCandidate(restored, output + "/existing-form-image-restored.pdf");
    const auto replacementPath = QDir(base).filePath(fixed["replacement"].toString());
    check(fileHash(replacementPath).toHex() == fixed["replacement_sha256"].toString().toLatin1(),
          "Frozen nested replacement SHA");
    const QImage replacement(replacementPath);
    auto changed = editExistingImage(source, 0, first.occurrence, ExistingImageChange::Replace,
                                     target, replacement, {}, true);
    retained(source, changed);
    find(changed, target);
    writeCandidate(changed, output + "/existing-form-image-replaced.pdf");
    Document history;
    history.open(path);
    history.commit(changed);
    history.undo();
    check(encodePdf(history.pdf()) == initial, "Nested image Undo exact original");
    history.redo();
    check(encodePdf(history.pdf()) == encodePdf(changed), "Nested image Redo exact candidate");
    history.save(output + "/existing-form-image-history.pdf");
    Document reopened;
    reopened.open(output + "/existing-form-image-history.pdf");
    reopened.commit(editExistingImage(reopened.pdf(), 0, find(reopened.pdf(), target).occurrence,
                                      ExistingImageChange::Geometry, original, {}, {}, true));
    retained(source, reopened.pdf());
    reopened.save(output + "/existing-form-image-reedited.pdf");
    auto removed = editExistingImage(source, 0, first.occurrence, ExistingImageChange::Remove, {},
                                     {}, {}, true);
    retained(source, removed);
    check(existingImages(removed, 0, {}, true).size() == 3,
          "Only selected nested invocation removed");
    writeCandidate(removed, output + "/existing-form-image-removed.pdf");
    int refused = 0;
    try
    {
        editExistingImage(source, 0, first.occurrence, ExistingImageChange::Geometry,
                          {350, 177, 48, 32}, {}, {}, true);
    }
    catch (const std::exception& e)
    {
        check(QString::fromUtf8(e.what()).contains("表示範囲"), "BBox reason");
        ++refused;
    }
    for (const auto& name :
         {QString("cyclic-form.pdf"), QString("group-form.pdf"), QString("clipped-form.pdf")})
        try
        {
            existingImages(readPdf(base + name), 0, {}, true);
        }
        catch (const std::exception&)
        {
            ++refused;
        }
    try
    {
        editExistingImage(
            source, 0, first.occurrence, ExistingImageChange::Remove, {}, {}, [] { return true; },
            true);
    }
    catch (const std::exception&)
    {
        ++refused;
    }
    check(refused == 5 && encodePdf(source) == initial,
          "Nested refusals atomic and original intact");
    QFile boundaryFile(base + "bbox-criteria.json");
    check(boundaryFile.open(QIODevice::ReadOnly), "Read fixed inclusive BBox criteria");
    const auto boundary = QJsonDocument::fromJson(boundaryFile.readAll()).object();
    const auto edge = bounds(boundary["edge_physical"].toArray());
    auto edgeDocument = editExistingImage(source, 0, first.occurrence,
                                          ExistingImageChange::Geometry, edge, {}, {}, true);
    retained(source, edgeDocument);
    find(edgeDocument, edge);
    writeCandidate(edgeDocument, output + "/existing-form-image-bbox-edge.pdf");
    bool outside = false;
    try
    {
        editExistingImage(source, 0, first.occurrence, ExistingImageChange::Geometry,
                          bounds(boundary["outside_physical"].toArray()), {}, {}, true);
    }
    catch (const std::exception& error)
    {
        outside = QString::fromUtf8(error.what()).contains("表示範囲");
    }
    check(outside && encodePdf(source) == initial,
          "0.01pt BBox overflow remains refused atomically");
    return {{"nested_shared_invocation_move_replace_remove", true},
            {"Undo_Redo_reopen_reedit", true},
            {"refusals", refused}};
}
QJsonObject testExistingFormImageUi(const QString& fixtures, const QString& output)
{
    const auto base = fixtures + "/existing-image-edit/forms/";
    QFile file(base + "ui-criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen grouped-image UI criteria");
    const auto fixed = QJsonDocument::fromJson(file.readAll()).object();
    const auto path = base + fixed["source"].toString();
    check(fileHash(path).toHex() == fixed["source_sha256"].toString().toLatin1(),
          "Frozen grouped UI source SHA");
    auto source = readPdf(path);
    const auto initial = encodePdf(source);
    ExistingImageDialog dialog(source, 0, {});
    dialog.show();
    auto preview =
        dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("existingImagePreview"));
    auto ready = [&]
    {
        check(QTest::qWaitFor([&] { return preview->property("renderReady").toBool(); }, 15000),
              "Grouped UI candidate ready");
    };
    ready();
    auto groups = dialog.findChild<QCheckBox*>("existingImageIncludeGroups");
    auto list = dialog.findChild<QListWidget*>("existingImageList");
    check(!groups->isChecked() && list->count() == 2, "Default direct mode unchanged");
    QTest::mouseClick(groups, Qt::LeftButton);
    ready();
    check(groups->isChecked() && list->count() == 4,
          "Actual optional group mode lists nested instances");
    const int row = fixed["selected_list_row"].toInt();
    list->setCurrentRow(row);
    ready();
    check(list->currentItem()->text().contains("2階層"), "Nested depth identifies grouped image");
    auto x = dialog.findChild<QDoubleSpinBox*>("existingImageX");
    auto y = dialog.findChild<QDoubleSpinBox*>("existingImageY");
    auto width = dialog.findChild<QDoubleSpinBox*>("existingImageWidth");
    auto height = dialog.findChild<QDoubleSpinBox*>("existingImageHeight");
    auto apply = dialog.findChild<QPushButton*>("applyExistingImage");
    const auto mm = fixed["numeric_mm"].toArray();
    x->setValue(mm[0].toDouble());
    y->setValue(mm[1].toDouble());
    width->setValue(mm[2].toDouble());
    height->setValue(mm[3].toDouble());
    ready();
    check(apply->isEnabled(), "Grouped numeric candidate can apply");
    x->setValue(250);
    check(QTest::qWaitFor(
              [&]
              {
                  return !apply->isEnabled() && dialog.findChild<QLabel*>("existingImageMessage")
                                                    ->text()
                                                    .contains("表示範囲");
              },
              15000),
          "BBox refusal finishes with an explicit reason");
    check(!apply->isEnabled() &&
              dialog.findChild<QLabel*>("existingImageMessage")->text().contains("表示範囲"),
          "BBox UI refuses without partial apply");
    x->setValue(mm[0].toDouble());
    ready();
    check(apply->isEnabled(), "Valid grouped candidate recovers after refusal");
    check(dialog.grab().save(output + "/existing-form-image-ui.png"),
          "Actual grouped-image dialog screenshot");
    apply->click();
    auto candidate = dialog.takeDocument();
    retained(source, candidate);
    find(candidate, bounds(fixed["physical_rectangle"].toArray()));
    writeCandidate(candidate, output + "/existing-form-image-ui.pdf");
    check(encodePdf(source) == initial, "Grouped UI original immutable");
    return {{"actual_optional_mode_selection_numeric_apply", true},
            {"BBox_refusal_and_recovery", true}};
}
} // namespace tatsu
