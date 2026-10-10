#include "redaction_candidate_checks.h"
#include "form_fields.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "redaction_document.h"
#include <limits>

namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        tatsu::fail(message);
}
pdf::PDFDocument withContent(const pdf::PDFDocument& document, const QByteArray& bytes)
{
    pdf::PDFDocumentBuilder builder(&document);
    const auto reference = document.getCatalog()->getPage(0)->getPageReference();
    auto dictionary = *document.getObjectByReference(reference).getDictionary();
    tatsu::detail::set(
        dictionary, "Contents",
        pdf::PDFObject::createReference(builder.addObject(tatsu::detail::streamObject({}, bytes))));
    builder.setObject(reference, tatsu::detail::dictObject(dictionary));
    return builder.build();
}
} // namespace
QJsonObject checkRedactionCandidate(pdf::PDFDocument document,
                                    const QMap<int, QVector<QRectF>>& regions,
                                    const QString& output)
{
    auto trace = [&](const QString& step)
    {
        QFile file(output + "/candidate-check-trace.txt");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append) ||
            file.write(step.toUtf8() + "\n") < 0 || !file.flush())
            tatsu::fail("Cannot record diagnostic checkpoint");
    };
    trace("encode source baseline");
    const auto originalBytes = tatsu::encodePdf(document);
    QJsonArray rejected;
    auto reject = [&](const QString& name, const std::function<void()>& operation)
    {
        trace("begin rejection: " + name);
        bool failed = false;
        try
        {
            operation();
        }
        catch (const std::exception&)
        {
            failed = true;
        }
        check(failed && tatsu::encodePdf(document) == originalBytes,
              "Unsafe case was not rejected atomically: " + name);
        rejected.append(name);
        trace("completed rejection: " + name);
    };
    for (const auto& [name, plan] : std::vector<std::pair<QString, QMap<int, QVector<QRectF>>>>{
             {"empty plan", {}},
             {"page outside", {{2, {{40, 40, 100, 100}}}}},
             {"empty region", {{0, {{40, 40, 0, 100}}}}},
             {"nonfinite region",
              {{0, {{std::numeric_limits<double>::quiet_NaN(), 40, 100, 100}}}}},
             {"outside page", {{0, {{-50, 40, 100, 100}}}}},
             {"partial glyph", {{0, {{60, 648, 5, 5}}}}},
             {"partial text command", {{0, {{59, 620, 45, 65}}}}},
             {"partial widget", {{0, {{60, 465, 60, 25}}}}},
             {"too many regions", {{0, QVector<QRectF>(1001, {40, 40, 10, 10})}}}})
        reject(name, [&] { tatsu::prepareRedactionCandidate(document, plan); });
    for (const auto& [name, bytes] : std::vector<std::pair<QString, QByteArray>>{
             {"inline image", "BI /W 1 /H 1 /BPC 8 /CS /RGB ID abc EI"},
             {"unknown command", "not_a_pdf_operator"},
             {"unbalanced graphics", "q 0 g"},
             {"intersecting vector", "100 280 80 40 re f"},
             {"marked content", "/Secret BMC EMC"}})
        reject(name,
               [&] { tatsu::prepareRedactionCandidate(withContent(document, bytes), regions); });
    reject("cancel before",
           [&] { tatsu::prepareRedactionCandidate(document, regions, [] { return true; }); });
    int observations = 0;
    trace("measure actual candidate processing");
    auto successful = tatsu::prepareRedactionCandidate(document, regions,
                                                       [&]
                                                       {
                                                           ++observations;
                                                           return false;
                                                       });
    check(observations > 20, "Actual processing cancellation checkpoints");
    int cancelledAt = 0;
    reject("cancel late",
           [&]
           {
               tatsu::prepareRedactionCandidate(document, regions,
                                                [&] { return ++cancelledAt >= observations - 2; });
           });
    check(cancelledAt == observations - 2,
          "Late cancellation actually reached candidate processing");
    trace("retain following text position in one text object");
    const auto following = withContent(
        document, "BT /F1 16 Tf 60 650 Td (SECRET_TEXT_9df67) Tj (KEEP_FOLLOW_9df67) Tj ET\n");
    tatsu::writeCandidate(following, output + "/text-advance-source.pdf");
    // Independent source boxes: final secret glyph ends at x=222.896; the
    // following K starts at x=224.784. End the selection inside that gap.
    const auto advance = tatsu::prepareRedactionCandidate(following, {{0, {{48, 620, 176, 65}}}});
    tatsu::writeCandidate(advance.document, output + "/sanitized-advance.pdf");
    trace("related comments popups and replies");
    pdf::PDFDocumentBuilder notes(&document);
    const auto pageReference = document.getCatalog()->getPage(0)->getPageReference();
    auto note = [&](const char* subtype, QRectF bounds, const char* contents)
    {
        pdf::PDFDictionary dictionary;
        tatsu::detail::set(dictionary, "Type", pdf::PDFObject::createName("Annot"));
        tatsu::detail::set(dictionary, "Subtype", pdf::PDFObject::createName(subtype));
        tatsu::detail::set(dictionary, "Rect", tatsu::detail::rectObject(bounds));
        tatsu::detail::set(dictionary, "P", pdf::PDFObject::createReference(pageReference));
        tatsu::detail::set(dictionary, "Contents", pdf::PDFObject::createString(contents));
        return dictionary;
    };
    auto deleted = note("Text", {100, 630, 20, 20}, "SECRET_NOTE_9df67");
    const auto deletedReference = notes.addObject(tatsu::detail::dictObject(deleted));
    auto popup = note("Popup", {480, 650, 100, 70}, "SECRET_POPUP_9df67");
    tatsu::detail::set(popup, "Parent", pdf::PDFObject::createReference(deletedReference));
    const auto popupReference = notes.addObject(tatsu::detail::dictObject(popup));
    tatsu::detail::set(deleted, "Popup", pdf::PDFObject::createReference(popupReference));
    notes.setObject(deletedReference, tatsu::detail::dictObject(deleted));
    auto reply = note("Text", {480, 580, 20, 20}, "SECRET_REPLY_9df67");
    tatsu::detail::set(reply, "IRT", pdf::PDFObject::createReference(deletedReference));
    const auto replyReference = notes.addObject(tatsu::detail::dictObject(reply));
    const auto keepReference = notes.addObject(
        tatsu::detail::dictObject(note("Text", {480, 500, 20, 20}, "KEEP_NOTE_9df67")));
    auto pageDictionary = *notes.getObjectByReference(pageReference).getDictionary();
    std::vector<pdf::PDFObject> annotations;
    for (const auto reference : document.getCatalog()->getPage(0)->getAnnotations())
        annotations.push_back(pdf::PDFObject::createReference(reference));
    for (const auto reference : {deletedReference, popupReference, replyReference, keepReference})
        annotations.push_back(pdf::PDFObject::createReference(reference));
    tatsu::detail::set(pageDictionary, "Annots", tatsu::detail::arrObject(annotations));
    notes.setObject(pageReference, tatsu::detail::dictObject(pageDictionary));
    const auto annotationSource = notes.build();
    tatsu::writeCandidate(annotationSource, output + "/annotation-source.pdf");
    const auto annotationCandidate = tatsu::prepareRedactionCandidate(annotationSource, regions);
    check(annotationCandidate.annotations == 4,
          "Selected widget, comment, outside popup and reply removed");
    check(annotationCandidate.document.getCatalog()->getPage(0)->getAnnotations().size() == 2,
          "Outside comment and form preserved");
    tatsu::writeCandidate(annotationCandidate.document, output + "/sanitized-annotations.pdf");
    tatsu::Document input;
    trace("unsaved Japanese signature");
    input.history = {document};
    input.putSignature(1, "墨消し前の未保存署名", {120, 80}, 18, Qt::black);
    const auto unsaved = input.pdf();
    const auto cursor = input.cursor;
    const auto candidate = tatsu::prepareRedactionCandidate(input.pdf(), regions);
    check(input.pdf() == unsaved && input.cursor == cursor && input.dirty(),
          "Original unsaved state preserved");
    const auto signatures = tatsu::signatures(candidate.document, 1);
    check(signatures.size() == 1 && signatures[0].text == "墨消し前の未保存署名",
          "Unselected editable signature preserved");
    tatsu::writeCandidate(candidate.document, output + "/sanitized-unsaved-signature.pdf");
    input.undo();
    check(input.pdf() == document, "Original Undo history remains independent");
    tatsu::Document reopened;
    trace("reopen sanitized candidate");
    reopened.open(output + "/sanitized-trial.pdf");
    auto fields = tatsu::formFields(reopened.pdf());
    check(fields.size() == 1 && fields[0].qualifiedName == "keep-field",
          "Only unselected form remains");
    tatsu::putFormValue(reopened, fields[0].widget, {"KEEP_REEDIT_9df67"});
    trace("save remaining form reinput");
    reopened.save(output + "/sanitized-reinput.pdf");
    const auto saved = tatsu::formFields(tatsu::readPdf(output + "/sanitized-reinput.pdf"));
    check(saved.size() == 1 && saved[0].values == QStringList{"KEEP_REEDIT_9df67"},
          "Saved candidate form reinput");
    check(tatsu::encodePdf(document) == originalBytes,
          "All processing leaves source PDF bytes unchanged");
    return QJsonObject{
        {"status", "PASS"},
        {"rejected_cases", rejected},
        {"late_cancel_observations", cancelledAt},
        {"source_preserved", true},
        {"unsaved_signature_and_original_Undo_preserved", true},
        {"remaining_form_saved_reinput", true},
        {"following_text_advance_executed", true},
        {"related_annotation_graph_executed", true},
        {"scope", "Experimental subset; no native UI or general security acceptance"}};
}
