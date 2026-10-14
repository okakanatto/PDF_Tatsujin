#include "redaction_shared.h"
#include "pdf_objects.h"
#include "redaction_resource_usage.h"
#include "redaction_standard_font.h"

namespace tatsu
{
namespace
{
using namespace pdf;
using namespace detail;
PDFDictionary resources(const RedactedPageContent& content)
{
    PDFDictionary result;
    set(result, "Font", dictObject(content.fonts));
    set(result, "XObject", dictObject(content.xobjects));
    set(result, "ColorSpace", dictObject(content.colorSpaces));
    set(result, "ExtGState", dictObject(content.graphicsStates));
    return result;
}
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("墨消しを中止しました。文書は変更していません。");
}
} // namespace
void rebuildSharedRedactionResources(
    const PDFDocument& source, const QMap<int, QVector<QRectF>>& rectangles,
    const std::set<PDFObjectReference>& deletedAnnotations,
    std::set<PDFObjectReference>& modifiedImages, std::set<PDFObjectReference>& removedDependencies,
    QVector<PDFObject>& directFonts, const std::set<PDFObjectReference>& forbiddenForms,
    const QMap<PDFObjectReference, QPainterPath>& formRegions, PDFDocumentBuilder& builder,
    const std::function<bool()>& cancelled, RedactionFontCvtUsage* cvtUsage)
{
    if (removedDependencies.empty() && directFonts.isEmpty())
        return;
    // Each pass starts from source bytes, never from an already recoded font.
    // Copied declarations may add new dependencies; converge before auditing.
    bool complete = false;
    for (int pass = 0; pass < 8 && !complete; ++pass)
    {
        const auto previous = removedDependencies;
        const auto previousDirectFonts = directFonts;
        RedactionResourceUsage usage(source, previous, forbiddenForms, formRegions,
                                     previousDirectFonts, cancelled);
        auto remember = [&](const RedactedPageContent& content)
        {
            removedDependencies.insert(content.removedResourceDependencies.begin(),
                                       content.removedResourceDependencies.end());
            modifiedImages.insert(content.modifiedImageSources.begin(),
                                  content.modifiedImageSources.end());
            mergeRemovedDirectRedactionFonts(directFonts, content.removedDirectFontDeclarations,
                                             cancelled);
        };
        for (size_t pageIndex = 0; pageIndex < source.getCatalog()->getPageCount(); ++pageIndex)
        {
            stop(cancelled);
            const auto page = source.getCatalog()->getPage(pageIndex);
            const auto reference = page->getPageReference();
            auto pageDictionary = *builder.getObjectByReference(reference).getDictionary();
            if (usage.affectedResources(page->getResources()))
            {
                const auto content =
                    redactPageContent(source, int(pageIndex), rectangles.value(int(pageIndex)),
                                      builder, cancelled, &usage, cvtUsage);
                remember(content);
                const auto original = source.getDictionaryFromObject(page->getResources());
                auto pageResources = *original;
                const auto rebuilt = resources(content);
                for (size_t i = 0; i < rebuilt.getCount(); ++i)
                    pageResources.setEntry(rebuilt.getKey(i), PDFObject(rebuilt.getValue(i)));
                for (const auto key : {"Pattern", "Shading", "Properties"})
                    pageResources.removeEntry(key);
                set(pageDictionary, "Resources", dictObject(pageResources));
                set(pageDictionary, "Contents",
                    PDFObject::createReference(builder.addObject(streamObject({}, content.bytes))));
            }
            else if (!pageDictionary.hasKey("Resources"))
                set(pageDictionary, "Resources", PDFObject(page->getResources()));
            builder.setObject(reference, dictObject(pageDictionary));
            for (const auto annotation : page->getAnnotations())
            {
                if (deletedAnnotations.contains(annotation))
                    continue;
                stop(cancelled);
                auto dictionary = *source.getObjectByReference(annotation).getDictionary();
                const auto originalAppearance = source.getObject(dictionary.get("AP"));
                if (originalAppearance.isNull())
                    continue;
                if (!originalAppearance.isDictionary())
                    fail("注釈外観の辞書が不正です。");
                auto appearance = *originalAppearance.getDictionary();
                bool changed = false;
                auto rebuild = [&](const PDFObject& original)
                {
                    const auto object = source.getObject(original);
                    if (!object.isStream())
                        fail("注釈外観の状態が不正です。");
                    const auto form = object.getStream()->getDictionary();
                    if (!usage.affectedResources(form->get("Resources")))
                        return PDFObject(original);
                    const auto content = reencodeRedactionAppearance(
                        source, int(pageIndex), original, builder, usage, cancelled, cvtUsage);
                    remember(content);
                    removedDependencies.insert(original.getReference());
                    PDFDictionary sanitized;
                    set(sanitized, "Type", PDFObject::createName("XObject"));
                    set(sanitized, "Subtype", PDFObject::createName("Form"));
                    set(sanitized, "BBox", PDFObject(form->get("BBox")));
                    if (form->hasKey("Matrix"))
                        set(sanitized, "Matrix", PDFObject(form->get("Matrix")));
                    set(sanitized, "Resources", dictObject(resources(content)));
                    changed = true;
                    return PDFObject::createReference(
                        builder.addObject(streamObject(sanitized, content.bytes)));
                };
                for (const auto key : {"N", "R", "D"})
                {
                    const auto value = source.getObject(appearance.get(key));
                    if (value.isNull())
                        continue;
                    if (value.isStream())
                        set(appearance, key, rebuild(appearance.get(key)));
                    else if (value.isDictionary())
                    {
                        auto states = *value.getDictionary();
                        for (size_t i = 0; i < states.getCount(); ++i)
                            states.setEntry(states.getKey(i), rebuild(states.getValue(i)));
                        set(appearance, key, dictObject(states));
                    }
                    else
                        fail("注釈外観の形式が不正です。");
                }
                if (changed)
                {
                    set(dictionary, "AP", dictObject(appearance));
                    builder.setObject(annotation, dictObject(dictionary));
                }
            }
        }
        complete = previous == removedDependencies && previousDirectFonts == directFonts;
    }
    if (!complete)
        fail("共有字体の参照が再構築の処理上限を超えています。候補は出力していません。");
    // Every leaf now has explicit resources. Inherited dictionaries otherwise
    // remain reachable through /Parent even when rendering no longer uses them.
    std::set<PDFObjectReference> parents;
    for (size_t pageIndex = 0; pageIndex < source.getCatalog()->getPageCount(); ++pageIndex)
    {
        auto parent =
            source.getObjectByReference(source.getCatalog()->getPage(pageIndex)->getPageReference())
                .getDictionary()
                ->get("Parent");
        std::set<PDFObjectReference> branch;
        while (!parent.isNull())
        {
            stop(cancelled);
            if (!parent.isReference() || !branch.insert(parent.getReference()).second ||
                branch.size() > 64)
                fail("ページ木の継承が不正または処理上限を超えています。");
            const auto reference = parent.getReference();
            const auto object = builder.getObjectByReference(reference);
            if (!object.isDictionary())
                fail("ページ木の辞書が不正です。");
            auto dictionary = *object.getDictionary();
            parent = dictionary.get("Parent");
            if (parents.insert(reference).second)
            {
                dictionary.removeEntry("Resources");
                builder.setObject(reference, dictObject(dictionary));
            }
        }
    }
}
} // namespace tatsu
