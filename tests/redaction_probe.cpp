#include "document.h"
#include "pdf_objects.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include "pdfoptimizer.h"
#include "pdfpagecontenteditorcontentstreambuilder.h"
#include "pdfpagecontenteditorprocessor.h"
#include "pdfredact.h"
#include "redaction_candidate_checks.h"
#include "redaction_content.h"
#include "redaction_document.h"
#include <QGuiApplication>

// Feasibility experiment only. These outputs are not safe redacted documents.
// Each strategy is evaluated independently; successful execution is not acceptance.
int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    const bool candidateOnly = arguments.size() == 5 && arguments[1] == "--candidate-only";
    if (arguments.size() != 4 && !candidateOnly)
        return 2;
    const int first = candidateOnly ? 2 : 1;
    const QString input = arguments[first], planPath = arguments[first + 1],
                  output = arguments[first + 2];
    QJsonObject result{{"scope", "Unsafe synthetic redaction feasibility outputs only"}};
    try
    {
        if (QFileInfo::exists(output) || !QDir().mkpath(output))
            tatsu::fail("Use a fresh diagnostic output directory");
        auto document = tatsu::readPdf(input);
        const auto originalHash = tatsu::fileHash(input);
        QFile planFile(planPath);
        if (!planFile.open(QIODevice::ReadOnly))
            tatsu::fail("Cannot read diagnostic plan");
        QMap<int, QPainterPath> regions;
        QMap<int, QVector<QRectF>> rectangles;
        for (const auto& row : QJsonDocument::fromJson(planFile.readAll()).array())
        {
            const auto entry = row.toObject();
            const auto rectangle = entry["rect"].toArray();
            const int page = entry["page"].toInt(-1);
            if (page < 0 || page >= int(document.getCatalog()->getPageCount()) ||
                rectangle.size() != 4)
                tatsu::fail("Invalid diagnostic plan");
            const QRectF bounds{rectangle[0].toDouble(), rectangle[1].toDouble(),
                                rectangle[2].toDouble(), rectangle[3].toDouble()};
            regions[page].addRect(bounds);
            rectangles[page].append(bounds);
        }
        if (candidateOnly)
        {
            const auto candidate = tatsu::prepareRedactionCandidate(document, rectangles);
            tatsu::writeCandidate(candidate.document, output + "/candidate.pdf");
            result["original_unchanged"] = tatsu::fileHash(input) == originalHash;
            result["status"] = "EXECUTED_NOT_ACCEPTED";
        }
        else
        {
            pdf::PDFDocumentBuilder marker(&document);
            for (auto it = rectangles.cbegin(); it != rectangles.cend(); ++it)
                for (const auto& rectangle : it.value())
                    marker.createAnnotationRedact(
                        document.getCatalog()->getPage(it.key())->getPageReference(), rectangle,
                        Qt::black);
            auto marked = marker.build();
            pdf::PDFFontCache fonts{128, 128};
            fonts.setDocument(pdf::PDFModifiedDocument(&marked, nullptr));
            pdf::PDFCMSManager manager(nullptr);
            const auto cms = manager.getCurrentCMS();
            const pdf::PDFMeshQualitySettings mesh;
            pdf::PDFRedact redact(&marked, &fonts, cms.data(), nullptr, &mesh, Qt::black);
            auto redacted = redact.perform(pdf::PDFRedact::None);
            tatsu::writeCandidate(redacted, output + "/upstream-redact.pdf");

            // Second trial: remove complete intersecting content elements. The editor
            // retains original resources, so this alone must never be called secure.
            auto edited = document;
            QJsonArray elements;
            bool editorValid = true;
            for (auto it = regions.cbegin(); it != regions.cend(); ++it)
            {
                const auto page = document.getCatalog()->getPage(it.key());
                fonts.setDocument(pdf::PDFModifiedDocument(&document, nullptr));
                pdf::PDFPageContentEditorProcessor processor(page, &document, &fonts, cms.data(),
                                                             nullptr, {}, mesh);
                const auto errors = processor.processContents();
                auto content = processor.takeEditedPageContent();
                pdf::PDFPageContentEditorContentStreamBuilder writer(&edited);
                writer.setFontDictionary(content.getFontDictionary());
                writer.setXObjectDictionary(content.getXObjectDictionary());
                writer.setGraphicStateDictionary(content.getGraphicStateDictionary());
                for (size_t i = 0; i < content.getElementCount(); ++i)
                {
                    const auto element = content.getElement(i);
                    const auto bounds = element->getBoundingBox();
                    const bool removed = it.value().intersects(bounds);
                    elements.append(
                        QJsonObject{{"page", it.key()},
                                    {"type", int(element->getType())},
                                    {"bounds", QJsonArray{bounds.x(), bounds.y(), bounds.width(),
                                                          bounds.height()}},
                                    {"removed", removed}});
                    if (!removed)
                        writer.writeEditedElement(element);
                }
                writer.writeStyledPath(it.value(), QPen(Qt::NoPen), QBrush(Qt::black), false, true);
                result[QString("editor_page_%1_writer_errors").arg(it.key())] =
                    QJsonArray::fromStringList(writer.getErrors());
                editorValid = editorValid && writer.getErrors().isEmpty();
                QJsonArray diagnostics;
                for (const auto& error : errors)
                    diagnostics.append(error.message);
                result[QString("editor_page_%1_diagnostics").arg(it.key())] = diagnostics;
                pdf::PDFDocumentBuilder builder(&edited);
                auto dictionary =
                    *edited.getObjectByReference(page->getPageReference()).getDictionary();
                pdf::PDFDictionary resources;
                tatsu::detail::set(resources, "Font",
                                   tatsu::detail::dictObject(writer.getFontDictionary()));
                tatsu::detail::set(resources, "XObject",
                                   tatsu::detail::dictObject(writer.getXObjectDictionary()));
                tatsu::detail::set(resources, "ExtGState",
                                   tatsu::detail::dictObject(writer.getGraphicStateDictionary()));
                tatsu::detail::set(dictionary, "Resources", tatsu::detail::dictObject(resources));
                tatsu::detail::set(
                    dictionary, "Contents",
                    pdf::PDFObject::createReference(builder.addObject(
                        tatsu::detail::streamObject({}, writer.getOutputContent()))));
                builder.setObject(page->getPageReference(), tatsu::detail::dictObject(dictionary));
                edited = builder.build();
            }
            // A writer error invalidates this strategy. Never serialize that candidate.
            auto optimized = edited;
            if (editorValid)
            {
                pdf::PDFOptimizer optimizer(pdf::PDFOptimizer::All, nullptr);
                optimizer.setDocument(&edited);
                optimizer.optimize();
                optimized = optimizer.takeOptimizedDocument();
                tatsu::writeCandidate(optimized, output + "/editor-trial.pdf");
            }
            result["editor_candidate_valid"] = editorValid;
            pdf::PDFDocumentBuilder contentBuilder(&document);
            QJsonArray contentChanges;
            for (auto it = rectangles.cbegin(); it != rectangles.cend(); ++it)
            {
                const auto content =
                    tatsu::redactPageContent(document, it.key(), it.value(), contentBuilder);
                const auto page = document.getCatalog()->getPage(it.key());
                auto dictionary =
                    *document.getObjectByReference(page->getPageReference()).getDictionary();
                auto resources = *document.getDictionaryFromObject(page->getResources());
                tatsu::detail::set(resources, "XObject",
                                   tatsu::detail::dictObject(content.xobjects));
                tatsu::detail::set(dictionary, "Resources", tatsu::detail::dictObject(resources));
                tatsu::detail::set(dictionary, "Contents",
                                   pdf::PDFObject::createReference(contentBuilder.addObject(
                                       tatsu::detail::streamObject({}, content.bytes))));
                contentBuilder.setObject(page->getPageReference(),
                                         tatsu::detail::dictObject(dictionary));
                contentChanges.append(
                    QJsonObject{{"page", it.key()},
                                {"removed_text_segments", content.removedTextSegments},
                                {"modified_images", content.modifiedImages}});
            }
            auto contentDocument = contentBuilder.build();
            pdf::PDFOptimizer contentOptimizer(pdf::PDFOptimizer::RemoveUnusedObjects |
                                                   pdf::PDFOptimizer::ShrinkObjectStorage,
                                               nullptr);
            contentOptimizer.setDocument(&contentDocument);
            contentOptimizer.optimize();
            contentDocument = contentOptimizer.takeOptimizedDocument();
            tatsu::writeCandidate(contentDocument, output + "/content-trial.pdf");
            result["content_changes"] = contentChanges;
            auto sanitized = tatsu::prepareRedactionCandidate(document, rectangles);
            tatsu::writeCandidate(sanitized.document, output + "/sanitized-trial.pdf");
            result["sanitized_changes"] = QJsonObject{{"text_segments", sanitized.textSegments},
                                                      {"images", sanitized.images},
                                                      {"field_groups", sanitized.fieldGroups},
                                                      {"annotations", sanitized.annotations}};
            result["candidate_checks"] = checkRedactionCandidate(document, rectangles, output);
            result["elements"] = elements;
            result["original_unchanged"] = tatsu::fileHash(input) == originalHash;
            result["input_sha256"] = QString::fromLatin1(originalHash.toHex());
            result["status"] = "EXECUTED_NOT_ACCEPTED";
            std::vector<pdf::PDFDocument*> renderDocuments{&document, &redacted};
            renderDocuments.push_back(&contentDocument);
            renderDocuments.push_back(&sanitized.document);
            if (editorValid)
                renderDocuments.push_back(&optimized);
            for (auto* pdf : renderDocuments)
            {
                const QString name = pdf == &document             ? "original"
                                     : pdf == &redacted           ? "upstream"
                                     : pdf == &contentDocument    ? "content"
                                     : pdf == &sanitized.document ? "sanitized"
                                                                  : "editor";
                QStringList errors;
                const auto image = tatsu::renderPage(*pdf, 0, 1.5, true, true,
                                                     tatsu::RenderPurpose::View, &errors);
                if (!image.save(output + "/" + name + ".png"))
                    tatsu::fail("Cannot save diagnostic rendering");
                result[name + "_render_errors"] = QJsonArray::fromStringList(errors);
                result[name + "_text"] = tatsu::pageText(*pdf, 0);
            }
        }
    }
    catch (const std::exception& error)
    {
        result["status"] = "FAIL";
        result["error"] = QString::fromUtf8(error.what());
    }
    QFile report(output + "/probe.json");
    if (!report.open(QIODevice::WriteOnly) || report.write(QJsonDocument(result).toJson()) < 0)
        return 3;
    return result["status"] == "FAIL" ? 1 : 0;
}
