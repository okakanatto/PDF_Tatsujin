#include "redaction_document.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfoptimizer.h"
#include "pdfsecurityhandler.h"
#include "redaction_content.h"
#include <algorithm>
#include <set>

namespace tatsu
{
namespace
{
using namespace pdf;
using namespace detail;
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("墨消しを中止しました。文書は変更していません。");
}
void removePrivatePageData(PDFDictionary& dictionary)
{
    for (const auto key : {"Metadata", "PieceInfo", "AF", "Thumb"})
        dictionary.removeEntry(key);
}
void rejectRetainedOriginalImages(const PDFObjectStorage& storage,
                                  const std::set<PDFObjectReference>& images,
                                  const std::function<bool()>& cancelled)
{
    if (images.empty())
        return;
    std::set<PDFObjectReference> visited;
    std::vector<PDFObject> pending{storage.getTrailerDictionary()};
    size_t examined = 0;
    auto appendDictionary = [&](const PDFDictionary* dictionary)
    {
        if (dictionary->getCount() > 1000000 - examined ||
            pending.size() + dictionary->getCount() > 1000000)
            fail("画像の共有参照が処理上限を超えています。墨消ししていません。");
        for (size_t i = 0; i < dictionary->getCount(); ++i)
            pending.push_back(dictionary->getValue(i));
    };
    // Walk the sanitized candidate's reachable graph, including unused named
    // resources and retained annotation appearances. Old storage orphans are
    // deliberately excluded; the optimizer removes them after this check.
    while (!pending.empty())
    {
        stop(cancelled);
        if (++examined > 1000000)
            fail("画像の共有参照が処理上限を超えています。墨消ししていません。");
        auto object = std::move(pending.back());
        pending.pop_back();
        if (object.isReference())
        {
            const auto reference = object.getReference();
            if (images.contains(reference))
                fail("墨消し対象の元画像が別の共有参照に残ります。このPDFはまだ安全に処理できません"
                     "。");
            if (visited.insert(reference).second)
                pending.push_back(storage.getObjectByReference(reference));
        }
        else if (object.isDictionary())
            appendDictionary(object.getDictionary());
        else if (object.isStream())
            appendDictionary(object.getStream()->getDictionary());
        else if (object.isArray())
        {
            const auto array = object.getArray();
            if (pending.size() + array->getCount() > 1000000)
                fail("画像の共有参照が処理上限を超えています。墨消ししていません。");
            for (const auto& item : *array)
                pending.push_back(item);
        }
    }
}
} // namespace
RedactionCandidate prepareRedactionCandidate(const PDFDocument& source,
                                             const QMap<int, QVector<QRectF>>& rectangles,
                                             const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (!source.getCatalog()->getPageCount() || !source.getStorage().getSecurityHandler() ||
        rectangles.isEmpty() || rectangles.size() > 1000)
        fail("PDFと墨消し範囲を確認してください。");
    const auto restriction = editingRestriction(source);
    if (!restriction.isEmpty())
        fail(restriction);
    if (source.getStorage().getSecurityHandler()->getMode() != EncryptionMode::None)
        fail("保護されたPDFの墨消しコピーにはまだ対応していません。保護は解除しません。");
    const auto rootReference = source.getTrailerDictionary()->get("Root");
    const auto rootObject = source.getObject(rootReference);
    if (!rootReference.isReference() || !rootObject.isDictionary())
        fail("PDFの文書構造が不正です。");
    auto catalog = *rootObject.getDictionary();
    for (const auto key : {"OCProperties", "StructTreeRoot", "AA", "OpenAction"})
        if (catalog.hasKey(key))
            fail("レイヤー、構造タグ、自動動作等があるPDFの安全な墨消しは未評価です。");
    QMap<int, QPainterPath> regions;
    for (auto it = rectangles.cbegin(); it != rectangles.cend(); ++it)
    {
        if (it.key() < 0 || it.key() >= int(source.getCatalog()->getPageCount()) ||
            it.value().isEmpty())
            fail("墨消しするページと範囲を確認してください。");
        regions[it.key()] = checkedRedactionRegions(source, it.key(), it.value());
    }
    PDFDocumentBuilder builder(&source);
    RedactionCandidate result;
    std::set<PDFObjectReference> modifiedImageSources;
    // Content validation runs before any annotation/field removal. All changes
    // stay in a private builder; cancellation never commits to the caller.
    for (auto it = rectangles.cbegin(); it != rectangles.cend(); ++it)
    {
        stop(cancelled);
        const auto content = redactPageContent(source, it.key(), it.value(), builder, cancelled);
        modifiedImageSources.insert(content.modifiedImageSources.begin(),
                                    content.modifiedImageSources.end());
        const auto page = source.getCatalog()->getPage(it.key());
        auto dictionary = *source.getObjectByReference(page->getPageReference()).getDictionary();
        const auto originalResources = source.getDictionaryFromObject(page->getResources());
        if (!originalResources)
            fail("ページのリソースが不正です。");
        auto resources = *originalResources;
        set(resources, "XObject", dictObject(content.xobjects));
        set(resources, "Font", dictObject(content.fonts));
        for (const auto key : {"ExtGState", "Pattern", "Shading", "Properties"})
            resources.removeEntry(key);
        set(dictionary, "Resources", dictObject(resources));
        set(dictionary, "Contents",
            PDFObject::createReference(builder.addObject(streamObject({}, content.bytes))));
        builder.setObject(page->getPageReference(), dictObject(dictionary));
        result.textSegments += content.removedTextSegments;
        result.images += content.modifiedImages;
    }
    PDFDocumentDataLoaderDecorator loader(&source);
    std::map<PDFObjectReference, int> annotationPages;
    std::set<PDFObjectReference> selectedAnnotations;
    for (size_t page = 0; page < source.getCatalog()->getPageCount(); ++page)
        for (const auto reference : source.getCatalog()->getPage(page)->getAnnotations())
        {
            stop(cancelled);
            if (!annotationPages.emplace(reference, int(page)).second ||
                annotationPages.size() > 100000)
                fail("注釈参照が重複または処理上限を超えています。");
            const auto object = source.getObjectByReference(reference);
            if (!object.isDictionary())
                fail("注釈の構造が不正です。");
            const auto bounds = loader.readRectangle(object.getDictionary()->get("Rect"), {});
            if (regions[int(page)].intersects(bounds))
            {
                if (!regions[int(page)].contains(bounds))
                    fail("注釈・フォームの一部だけの墨消しは未対応です。候補を確定していません。");
                selectedAnnotations.insert(reference);
            }
        }
    const auto formObject = source.getObject(source.getCatalog()->getFormObject());
    if (!formObject.isNull() && !formObject.isDictionary())
        fail("フォーム構造が不正です。");
    if (formObject.isDictionary())
    {
        auto form = *formObject.getDictionary();
        if (form.hasKey("XFA") || form.hasKey("CO"))
            fail("XFAや計算フォームの安全な墨消しは未評価です。");
        std::set<PDFObjectReference> visited;
        std::function<void(PDFObjectReference, int, std::set<PDFObjectReference>&)> scan =
            [&](PDFObjectReference reference, int depth, std::set<PDFObjectReference>& widgets)
        {
            stop(cancelled);
            if (depth > 32 || !visited.insert(reference).second || visited.size() > 100000)
                fail("フォーム参照が循環・重複または処理上限を超えています。");
            const auto object = source.getObjectByReference(reference);
            if (!object.isDictionary())
                fail("フォーム項目が不正です。");
            const auto dictionary = object.getDictionary();
            const auto subtype = source.getObject(dictionary->get("Subtype"));
            if (dictionary->hasKey("AA") ||
                source.getObject(dictionary->get("FT")) == PDFObject::createName("Sig"))
                fail("自動動作や証明書署名項目があるフォームの墨消しは未評価です。");
            if (subtype.isName() && subtype.getString() == "Widget")
            {
                if (!annotationPages.contains(reference))
                    fail("フォーム部品とページ注釈が一致しません。");
                widgets.insert(reference);
            }
            const auto kids = source.getObject(dictionary->get("Kids"));
            if (!kids.isNull() && !kids.isArray())
                fail("フォーム階層が不正です。");
            if (kids.isArray())
                for (const auto& kid : *kids.getArray())
                {
                    if (!kid.isReference())
                        fail("フォーム子項目の参照が不正です。");
                    scan(kid.getReference(), depth + 1, widgets);
                }
        };
        const auto fields = source.getObject(form.get("Fields"));
        if (!fields.isArray())
            fail("フォーム項目一覧が不正です。");
        std::vector<PDFObject> retained;
        for (const auto& field : *fields.getArray())
        {
            if (!field.isReference())
                fail("フォーム項目の参照が不正です。");
            std::set<PDFObjectReference> widgets;
            scan(field.getReference(), 0, widgets);
            const int selected =
                int(std::count_if(widgets.begin(), widgets.end(), [&](PDFObjectReference reference)
                                  { return selectedAnnotations.contains(reference); }));
            if (selected && selected != int(widgets.size()))
                fail("同じフォーム項目の範囲外の部品を保持できません。全項目の墨消しが必要です。");
            if (selected)
                ++result.fieldGroups;
            else
                retained.push_back(field);
        }
        set(form, "Fields", arrObject(retained));
        const auto reference = source.getCatalog()->getFormObject();
        if (reference.isReference())
            builder.setObject(reference.getReference(), dictObject(form));
        else
            set(catalog, "AcroForm", dictObject(form));
    }
    // Remove associated popups/replies together with their deleted annotation.
    std::map<PDFObjectReference, std::vector<PDFObjectReference>> dependents;
    for (const auto& [reference, page] : annotationPages)
    {
        stop(cancelled);
        const auto dictionary = source.getObjectByReference(reference).getDictionary();
        for (const auto key : {"Parent", "IRT"})
        {
            const auto parent = dictionary->get(key);
            if (parent.isReference())
                dependents[parent.getReference()].push_back(reference);
        }
        const auto popup = dictionary->get("Popup");
        if (popup.isReference() && annotationPages.contains(popup.getReference()))
            dependents[reference].push_back(popup.getReference());
    }
    std::vector<PDFObjectReference> pending(selectedAnnotations.begin(), selectedAnnotations.end());
    for (size_t i = 0; i < pending.size(); ++i)
    {
        stop(cancelled);
        const auto found = dependents.find(pending[i]);
        if (found == dependents.end())
            continue;
        for (const auto reference : found->second)
            if (selectedAnnotations.insert(reference).second)
                pending.push_back(reference);
    }
    for (size_t page = 0; page < source.getCatalog()->getPageCount(); ++page)
    {
        stop(cancelled);
        const auto reference = source.getCatalog()->getPage(page)->getPageReference();
        auto dictionary = *builder.getObjectByReference(reference).getDictionary();
        std::vector<PDFObject> retained;
        for (const auto annotation : source.getCatalog()->getPage(page)->getAnnotations())
            if (selectedAnnotations.contains(annotation))
                ++result.annotations;
            else
                retained.push_back(PDFObject::createReference(annotation));
        set(dictionary, "Annots", arrObject(retained));
        removePrivatePageData(dictionary);
        builder.setObject(reference, dictObject(dictionary));
    }
    const auto namesObject = source.getObject(catalog.get("Names"));
    if (!namesObject.isNull())
    {
        if (!namesObject.isDictionary() || namesObject.getDictionary()->hasKey("JavaScript"))
            fail("名前一覧やJavaScriptの安全な墨消しは未評価です。");
        auto names = *namesObject.getDictionary();
        names.removeEntry("EmbeddedFiles");
        set(catalog, "Names", dictObject(names));
    }
    for (const auto key : {"Metadata", "PieceInfo", "AF"})
        catalog.removeEntry(key);
    builder.setObject(rootReference.getReference(), dictObject(catalog));
    auto candidate = builder.build();
    auto storage = candidate.getStorage();
    auto trailer = *candidate.getTrailerDictionary();
    trailer.removeEntry("Info");
    trailer.removeEntry("Prev");
    storage.setTrailerDictionary(dictObject(trailer));
    rejectRetainedOriginalImages(storage, modifiedImageSources, cancelled);
    PDFOptimizer optimizer(PDFOptimizer::RemoveUnusedObjects | PDFOptimizer::ShrinkObjectStorage,
                           nullptr);
    optimizer.setStorage(storage);
    QObject::connect(
        &optimizer, &PDFOptimizer::optimizationProgress, &optimizer,
        [&](const QString&) { stop(cancelled); }, Qt::DirectConnection);
    optimizer.optimize();
    stop(cancelled);
    result.document =
        PDFDocument(optimizer.takeStorage(), source.getInfo()->version, source.getSourceDataHash());
    return result;
}
} // namespace tatsu
