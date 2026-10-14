#include "content_invocation_edit.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"

namespace tatsu
{
using namespace detail;
void spliceContentInvocation(PDFDocumentBuilder& builder, const PDFDocument& snapshot,
                             QVector<ContentInvocationOwner> owners, int selected, ContentSpan span,
                             QByteArray replacement, const std::function<void()>& checkCancelled)
{
    if (selected < 0 || selected >= owners.size())
        fail("編集する描画の所有者を確認できません。");
    while (true)
    {
        if (checkCancelled)
            checkCancelled();
        const auto& owner = owners[selected];
        if (span.begin < 0 || span.end < span.begin || span.end > owner.bytes.size())
            fail("編集する描画命令の範囲を確認できません。");
        const auto bytes = owner.bytes.left(span.begin) + replacement + owner.bytes.mid(span.end);
        auto attributes = owner.attributes;
        set(attributes, "Resources", dictObject(owner.resources));
        if (selected == 0)
        {
            set(attributes, "Contents",
                PDFObject::createReference(builder.addObject(streamObject({}, bytes))));
            builder.setObject(owner.reference, dictObject(attributes));
            return;
        }
        if (owner.parent < 0 || owner.parent >= selected)
            fail("描画グループの親子関係を確認できません。");
        attributes.removeEntry("Filter");
        attributes.removeEntry("DecodeParms");
        const auto reference = builder.addObject(streamObject(attributes, bytes));
        span = owner.parentSpan;
        selected = owner.parent;
        auto& resources = owners[selected].resources;
        const auto objects = snapshot.getObject(resources.get("XObject"));
        if (!objects.isDictionary())
            fail("親グループの描画資源を確認できません。");
        auto xobjects = *objects.getDictionary();
        int number = 1;
        QByteArray name;
        do
        {
            name = "TatsujinForm" + QByteArray::number(number++);
        } while (xobjects.hasKey(name));
        xobjects.setEntry(PDFInplaceOrMemoryString(name), PDFObject::createReference(reference));
        set(resources, "XObject", dictObject(xobjects));
        replacement = '/' + name + " Do";
    }
}
} // namespace tatsu
