#include "existing_image_tests.h"
#include "existing_image_edit.h"
#include "form_fields.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
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
QJsonObject criteria(const QString& fixtures)
{
    QFile file(fixtures + "/existing-image-edit/criteria.json");
    check(file.open(QIODevice::ReadOnly), "Read frozen image edit criteria");
    return QJsonDocument::fromJson(file.readAll()).object();
}
QRectF rectangle(const QJsonArray& array)
{
    check(array.size() == 4, "Fixed image edit rectangle");
    return {array[0].toDouble(), array[1].toDouble(), array[2].toDouble(), array[3].toDouble()};
}
PDFDocument source(const QString& fixtures)
{
    const auto base = fixtures + "/existing-image-edit/";
    const auto expected = criteria(fixtures)["files"].toObject();
    for (auto entry = expected.begin(); entry != expected.end(); ++entry)
        check(fileHash(base + entry.key()).toHex() == entry.value().toString().toLatin1(),
              "Frozen image input hash");
    return readPdf(base + "shared-images.pdf");
}
void sameText(PDFDocument before, PDFDocument after)
{
    for (int page = 0; page < int(before.getCatalog()->getPageCount()); ++page)
        check(pageText(before, page) == pageText(after, page), "All original page text preserved");
    const auto fields = formFields(after);
    check(fields.size() == 2, "Existing forms retained");
    for (const auto& field : fields)
        check(field.values == QStringList{field.qualifiedName == "keep-field"
                                              ? "KEEP_FORM_9df67"
                                              : "SECRET_FORM_9df67"},
              "Existing form values retained");
}
bool close(QRectF first, QRectF second)
{
    return qAbs(first.x() - second.x()) < 1e-7 && qAbs(first.y() - second.y()) < 1e-7 &&
           qAbs(first.width() - second.width()) < 1e-7 &&
           qAbs(first.height() - second.height()) < 1e-7;
}
} // namespace
QJsonObject testExistingImageCore(const QString& fixtures, const QString& output)
{
    const auto original = source(fixtures);
    const auto before = encodePdf(original);
    const auto images = existingImages(original, 0);
    const auto rotated = existingImages(original, 1);
    check(images.size() == 2 && rotated.size() == 1 && existingImages(original, 2).isEmpty(),
          "Direct shared-image occurrences enumerated");
    check(images[0].reference == images[1].reference && images[0].reference == rotated[0].reference,
          "Three drawings share one source image");
    const auto target = pageMatrix(original.getCatalog()->getPage(0))
                            .mapRect(rectangle(criteria(fixtures)["move_target"].toArray()));
    Document document;
    document.open(fixtures + "/existing-image-edit/shared-images.pdf");
    document.commit(editExistingImage(document.pdf(), 0, images[0].occurrence,
                                      ExistingImageChange::Geometry, target));
    check(document.cursor == 1 && document.dirty(), "One image edit is one Undo unit");
    const auto moved = encodePdf(document.pdf());
    auto changed = existingImages(document.pdf(), 0);
    check(close(changed[0].physical, target) && changed[1].matrix == images[1].matrix &&
              existingImages(document.pdf(), 1)[0].matrix == rotated[0].matrix,
          "Selected occurrence moves; shared occurrences remain unchanged");
    sameText(original, document.pdf());
    document.undo();
    check(encodePdf(document.pdf()) == before, "Image Undo restores original snapshot");
    document.redo();
    check(encodePdf(document.pdf()) == moved, "Image Redo restores edited snapshot");
    document.save(output + "/existing-image-moved.pdf");
    Document reopened;
    reopened.open(output + "/existing-image-moved.pdf");
    check(close(existingImages(reopened.pdf(), 0)[0].physical, target),
          "Saved image geometry retained");
    reopened.commit(
        editExistingImage(reopened.pdf(), 0, 0, ExistingImageChange::Geometry, images[0].physical));
    reopened.save(output + "/existing-image-restored.pdf");
    check(close(existingImages(reopened.pdf(), 0)[0].physical, images[0].physical),
          "Saved ordinary PDF image reedited");
    for (const auto& format : {QString("png"), QString("jpg")})
    {
        QImage replacement(fixtures + "/existing-image-edit/replacement." + format);
        check(!replacement.isNull(), "Fixed replacement image decoded");
        const auto replaced = editExistingImage(original, 0, 0, ExistingImageChange::Replace,
                                                images[0].physical, replacement);
        const auto rows = existingImages(replaced, 0);
        check(rows[0].reference != rows[1].reference && rows[1].reference == images[1].reference &&
                  rows[1].matrix == images[1].matrix,
              "Replacement uses a private resource for one occurrence");
        sameText(original, replaced);
        writeCandidate(replaced, output + "/existing-image-replaced-" + format + ".pdf");
    }
    const auto removed = editExistingImage(original, 0, 0, ExistingImageChange::Remove);
    check(existingImages(removed, 0).size() == 1 &&
              existingImages(removed, 0)[0].matrix == images[1].matrix,
          "Only selected drawing removed");
    sameText(original, removed);
    writeCandidate(removed, output + "/existing-image-removed.pdf");
    const auto rotatedTarget =
        pageMatrix(original.getCatalog()->getPage(1))
            .mapRect(rectangle(criteria(fixtures)["rotated_target"].toArray()));
    const auto rotatedCopy =
        editExistingImage(original, 1, 0, ExistingImageChange::Geometry, rotatedTarget);
    check(close(existingImages(rotatedCopy, 1)[0].physical, rotatedTarget),
          "Rotated CropBox/UserUnit image moves correctly");
    sameText(original, rotatedCopy);
    writeCandidate(rotatedCopy, output + "/existing-image-rotated.pdf");
    check(encodePdf(original) == before, "Immutable source snapshot unchanged");
    source(fixtures);
    return {{"moved_reopened_reedited", true},
            {"PNG_JPEG_replacement", true},
            {"only_selected_occurrence_removed", true},
            {"shared_images_text_forms_geometry_history_preserved", true}};
}
QJsonObject testExistingImageRejections(const QString& fixtures, const QString&)
{
    const auto original = source(fixtures);
    const auto before = encodePdf(original);
    int rejected = 0;
    auto reject = [&](const std::function<void()>& action, const QString& term)
    {
        bool expected = false;
        try
        {
            action();
        }
        catch (const std::exception& error)
        {
            expected = QString::fromUtf8(error.what()).contains(term);
        }
        check(expected && encodePdf(original) == before, "Specific refusal retains original");
        ++rejected;
    };
    reject([&] { editExistingImage(original, -1, 0, ExistingImageChange::Remove); }, "ページ");
    reject([&] { editExistingImage(original, 0, 99, ExistingImageChange::Remove); }, "対象");
    reject([&]
           { editExistingImage(original, 0, 0, ExistingImageChange::Geometry, {-5, 0, 20, 20}); },
           "範囲内");
    reject([&]
           { editExistingImage(original, 0, 0, ExistingImageChange::Geometry, {10, 10, 0, 20}); },
           "範囲内");
    reject(
        [&]
        {
            editExistingImage(original, 0, 0, ExistingImageChange::Geometry,
                              {std::numeric_limits<double>::quiet_NaN(), 10, 20, 20});
        },
        "範囲内");
    reject(
        [&]
        {
            editExistingImage(original, 0, 0, ExistingImageChange::Replace,
                              existingImages(original, 0)[0].physical);
        },
        "PNG/JPEG");
    reject([&] { editExistingImage(original, 0, 0, static_cast<ExistingImageChange>(99)); },
           "操作");
    reject(
        [&] {
            editExistingImage(original, 0, 0, ExistingImageChange::Remove, {}, {},
                              [] { return true; });
        },
        "中止");
    int cancellation = 0;
    reject(
        [&]
        {
            editExistingImage(original, 0, 0, ExistingImageChange::Remove, {}, {},
                              [&] { return ++cancellation > 5; });
        },
        "中止");
    auto clipped = original;
    pdf::PDFDocumentBuilder builder(&clipped);
    const auto page = clipped.getCatalog()->getPage(0);
    const auto bytes = clipped.getDecodedStream(clipped.getObject(page->getContents()).getStream());
    auto dictionary = *clipped.getObjectByReference(page->getPageReference()).getDictionary();
    detail::set(dictionary, "Contents",
                pdf::PDFObject::createReference(builder.addObject(
                    detail::streamObject({}, "q 60 250 240 120 re W n\n" + bytes + "\nQ"))));
    builder.setObject(page->getPageReference(), detail::dictObject(dictionary));
    clipped = builder.build();
    reject([&] { editExistingImage(clipped, 0, 0, ExistingImageChange::Remove); }, "クリッピング");
    QFile ocrCriteria(fixtures + "/existing-image-edit/ocr-refusal.json");
    check(ocrCriteria.open(QIODevice::ReadOnly), "Read fixed actual OCR refusal input");
    const auto fixedOcr = QJsonDocument::fromJson(ocrCriteria.readAll()).object();
    const auto ocrPath = fixtures + "/existing-image-edit/" + fixedOcr["file"].toString();
    check(fileHash(ocrPath).toHex() == fixedOcr["sha256"].toString().toLatin1(),
          "Fixed actual OCR hash");
    const auto ocr = readPdf(ocrPath);
    reject([&] { editExistingImage(ocr, 0, 0, ExistingImageChange::Remove); }, "OCR");
    return {{"specific_refusals", rejected}, {"original_unchanged", true}};
}
} // namespace tatsu
