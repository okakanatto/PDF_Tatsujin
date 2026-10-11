#include "page_operations.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentmanipulator.h"
#include "pdfform.h"
#include <set>

namespace tatsu
{
using namespace detail;
namespace
{
void validatePages(const PDFDocument& document, const QVector<int>& pages)
{
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    if (pages.isEmpty())
        fail("最後の1ページは削除できません。ページを指定してください。");
    QSet<int> seen;
    for (int page : pages)
    {
        if (page < 0 || page >= int(document.getCatalog()->getPageCount()) || seen.contains(page))
            fail("ページ範囲が不正か重複しています。");
        seen.insert(page);
    }
}
PDFDocument filterForm(const PDFDocument& document, const QVector<int>& pages)
{
    const auto form = document.getObject(document.getCatalog()->getFormObject());
    if (!form.isDictionary())
        return document;
    PDFDocumentBuilder builder(&document);
    std::set<PDFObjectReference> widgets, active;
    for (int page : pages)
    {
        const auto object =
            document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
        const auto annotations = document.getObject(object.getDictionary()->get("Annots"));
        if (annotations.isArray())
            for (const auto& annotation : *annotations.getArray())
                if (annotation.isReference())
                    widgets.insert(annotation.getReference());
    }
    std::function<bool(PDFObjectReference, int)> retain =
        [&](PDFObjectReference reference, int depth)
    {
        if (depth > 128 || active.contains(reference))
            fail("フォームの階層が不正です。");
        active.insert(reference);
        const auto object = document.getObjectByReference(reference);
        if (!object.isDictionary())
            fail("フォームの参照が不正です。");
        auto dictionary = *object.getDictionary();
        auto subtype = document.getObject(dictionary.get("Subtype"));
        if (subtype.isName() && subtype.getString() == "Widget")
        {
            active.erase(reference);
            return widgets.contains(reference);
        }
        auto kids = document.getObject(dictionary.get("Kids"));
        std::vector<PDFObject> kept;
        if (kids.isArray())
            for (const auto& kid : *kids.getArray())
                if (kid.isReference() && retain(kid.getReference(), depth + 1))
                    kept.push_back(kid);
        active.erase(reference);
        if (kept.empty())
            return false;
        set(dictionary, "Kids", arrObject(kept));
        builder.setObject(reference, dictObject(dictionary));
        return true;
    };
    const auto fields = document.getObject(form.getDictionary()->get("Fields"));
    std::vector<PDFObject> retained;
    if (fields.isArray())
        for (const auto& field : *fields.getArray())
            if (field.isReference() && retain(field.getReference(), 0))
                retained.push_back(field);
    auto dictionary = *form.getDictionary();
    set(dictionary, "Fields", arrObject(retained));
    auto object = dictObject(dictionary);
    const auto formReference = document.getCatalog()->getFormObject();
    if (formReference.isReference())
        builder.setObject(formReference.getReference(), object);
    else
        builder.setCatalogAcroForm(builder.addObject(object));
    return builder.build();
}
void checkCollisions(const QVector<PDFDocument>& documents)
{
    QSet<QString> allNames;
    for (const auto& document : documents)
    {
        auto form = PDFForm::parse(&document, document.getCatalog()->getFormObject());
        QSet<QString> names;
        form.apply(
            [&](const PDFFormField* field)
            {
                const auto name = field->getName(PDFFormField::FullyQualified);
                if (!name.isEmpty())
                {
                    if (allNames.contains(name))
                        fail("フォーム名が衝突するため結合できません: " + name +
                             "。入力が別ページへ連動することを防ぐため、文書は変更していません。");
                    names.insert(name);
                }
            });
        allNames.unite(names);
    }
}
PDFDocument assemble(const QVector<PDFDocument>& documents,
                     const PDFDocumentManipulator::AssembledPages& pages)
{
    for (const auto& document : documents)
    {
        const auto restriction = editingRestriction(document);
        if (!restriction.isEmpty())
            fail(restriction);
    }
    checkCollisions(documents);
    PDFDocumentManipulator manipulator;
    manipulator.setOutlineMode(PDFDocumentManipulator::OutlineMode::Join);
    for (int i = 0; i < documents.size(); ++i)
        manipulator.addDocument(i, &documents[i]);
    auto result = manipulator.assemble(pages);
    if (!result)
        fail("ページ操作に失敗しました: " + result.getErrorMessage());
    auto document = manipulator.takeAssembledDocument();
    if (document.getCatalog()->getPageCount() != pages.size())
        fail("ページ操作後のページ数が不正です。");
    return document;
}
} // namespace
PDFDocument selectPages(const PDFDocument& document, const QVector<int>& order)
{
    validatePages(document, order);
    auto filtered = filterForm(document, order);
    auto originals = PDFDocumentManipulator::createAllDocumentPages(0, &filtered);
    PDFDocumentManipulator::AssembledPages pages;
    for (int page : order)
        pages.push_back(originals[page]);
    return assemble({filtered}, pages);
}
PDFDocument insertPages(const PDFDocument& document, const PDFDocument& incoming,
                        const QVector<int>& selected, int before)
{
    validatePages(incoming, selected);
    const int count = int(document.getCatalog()->getPageCount());
    if (before < 0 || before > count)
        fail("挿入位置が不正です。");
    auto filtered = filterForm(incoming, selected);
    auto pages = PDFDocumentManipulator::createAllDocumentPages(0, &document);
    auto incomingPages = PDFDocumentManipulator::createAllDocumentPages(1, &filtered);
    PDFDocumentManipulator::AssembledPages added;
    for (int page : selected)
        added.push_back(incomingPages[page]);
    pages.insert(pages.begin() + before, added.begin(), added.end());
    return assemble({document, filtered}, pages);
}
PDFDocument mergeDocuments(const QVector<PDFDocument>& documents)
{
    if (documents.size() < 2)
        fail("結合するPDFを2つ以上指定してください。");
    PDFDocumentManipulator::AssembledPages pages;
    for (int i = 0; i < documents.size(); ++i)
    {
        auto added = PDFDocumentManipulator::createAllDocumentPages(i, &documents[i]);
        pages.insert(pages.end(), added.begin(), added.end());
    }
    return assemble(documents, pages);
}
QVector<int> movePageOrder(int count, QVector<int> selected, int before)
{
    if (selected.isEmpty() || before < 0 || before > count)
        fail("移動するページと位置を指定してください。");
    std::sort(selected.begin(), selected.end());
    QSet<int> seen;
    for (int page : selected)
    {
        if (page < 0 || page >= count || seen.contains(page))
            fail("移動するページが不正です。");
        seen.insert(page);
    }
    QVector<int> result;
    bool inserted = false;
    for (int page = 0; page <= count; ++page)
    {
        if (page == before)
        {
            result += selected;
            inserted = true;
        }
        if (page < count && !seen.contains(page))
            result.append(page);
    }
    if (!inserted)
        fail("移動位置が不正です。");
    return result;
}
QVector<ExportResult> exportPageGroups(const PDFDocument& document,
                                       const QVector<QVector<int>>& groups,
                                       const QStringList& destinations)
{
    if (groups.isEmpty() || groups.size() != destinations.size())
        fail("出力範囲と保存先を指定してください。");
    QSet<int> seen;
    for (const auto& group : groups)
    {
        validatePages(document, group);
        for (int page : group)
        {
            if (seen.contains(page))
                fail("出力範囲に重複ページがあります。");
            seen.insert(page);
        }
    }
    for (int i = 0; i < destinations.size(); ++i)
        for (int j = 0; j < i; ++j)
            if (sameFilePath(destinations[i], destinations[j]))
                fail("出力先が重複しています。");
    QVector<ExportResult> results;
    for (int i = 0; i < groups.size(); ++i)
    {
        ExportResult result{QFileInfo(destinations[i]).absoluteFilePath(), {}, false};
        try
        {
            Document output;
            output.history = {selectPages(document, groups[i])};
            output.saved = -1;
            output.save(result.path);
            result.success = true;
        }
        catch (const std::exception& error)
        {
            result.error = QString::fromUtf8(error.what());
        }
        results.append(std::move(result));
    }
    return results;
}
} // namespace tatsu
