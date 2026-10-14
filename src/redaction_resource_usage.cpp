#include "redaction_resource_usage.h"
#include "redaction_resource_audit.h"

namespace tatsu
{
using namespace pdf;
RedactionResourceUsage::RedactionResourceUsage(const PDFDocument& source,
                                               std::set<PDFObjectReference> removed,
                                               std::set<PDFObjectReference> forbiddenForms,
                                               QMap<PDFObjectReference, QPainterPath> formRegions,
                                               QVector<PDFObject> directFonts,
                                               std::function<bool()> cancelled)
    : source(source), removed(std::move(removed)), forbiddenForms(std::move(forbiddenForms)),
      directFonts(std::move(directFonts)), cancelled(std::move(cancelled))
{
    for (const auto& reference : this->removed)
    {
        stop();
        const auto object = source.getObjectByReference(reference);
        if (object.isStream())
        {
            const auto hash = redactionStreamFingerprint(source.getStorage(), object.getStream(),
                                                         decodedTotal, this->cancelled);
            hashes[reference] = hash;
            fingerprints.insert(hash);
            if (this->forbiddenForms.contains(reference))
            {
                forbiddenFormFingerprints.insert(hash);
                if (formRegions.contains(reference))
                {
                    if (formRegionsByHash.contains(hash) &&
                        formRegionsByHash[hash] != formRegions[reference])
                        fail("同じForm内容への異なる範囲指定は安全に処理できません。候補は出力して"
                             "いません。");
                    formRegionsByHash[hash] = formRegions[reference];
                }
            }
        }
    }
}
void RedactionResourceUsage::stop() const
{
    if (cancelled && cancelled())
        fail("墨消しを中止しました。文書は変更していません。");
}
bool RedactionResourceUsage::forbiddenForm(const PDFObject& form) const
{
    if (form.isReference() && forbiddenForms.contains(form.getReference()))
        return true;
    if (forbiddenFormFingerprints.empty())
        return false;
    const auto object = source.getObject(form);
    if (!object.isStream() || source.getObject(object.getStream()->getDictionary()->get(
                                  "Subtype")) != PDFObject::createName("Form"))
        return false;
    QByteArray hash;
    if (form.isReference() && hashes.contains(form.getReference()))
        hash = hashes.at(form.getReference());
    else
    {
        hash = redactionStreamFingerprint(source.getStorage(), object.getStream(), decodedTotal,
                                          cancelled);
        if (form.isReference())
            hashes[form.getReference()] = hash;
    }
    return forbiddenFormFingerprints.contains(hash);
}
bool RedactionResourceUsage::affectedFont(const PDFObject& declaration) const
{
    if (directFonts.contains(source.getObject(declaration)))
        return true;
    if (declaration.isReference())
        if (const auto found = fonts.find(declaration.getReference()); found != fonts.end())
            return found->second;
    std::vector<PDFObject> pending{declaration};
    std::set<PDFObjectReference> visited;
    size_t examined = 0;
    bool matched = false;
    while (!pending.empty() && !matched)
    {
        stop();
        if (++examined > 4096 || pending.size() > 4096)
            fail("共有字体の参照が処理上限を超えています。");
        auto object = std::move(pending.back());
        pending.pop_back();
        PDFObjectReference reference;
        if (object.isReference())
        {
            reference = object.getReference();
            if (removed.contains(reference))
            {
                matched = true;
                break;
            }
            if (!visited.insert(reference).second)
                continue;
            object = source.getObjectByReference(reference);
        }
        const PDFDictionary* dictionary = nullptr;
        if (object.isStream())
        {
            QByteArray hash;
            if (const auto found = hashes.find(reference);
                reference.isValid() && found != hashes.end())
                hash = found->second;
            else
            {
                hash = redactionStreamFingerprint(source.getStorage(), object.getStream(),
                                                  decodedTotal, cancelled);
                if (reference.isValid())
                    hashes[reference] = hash;
            }
            matched = fingerprints.contains(hash);
            dictionary = object.getStream()->getDictionary();
        }
        else if (object.isDictionary())
            dictionary = object.getDictionary();
        if (dictionary)
        {
            if (dictionary->getCount() + pending.size() > 4096)
                fail("共有字体の参照が処理上限を超えています。");
            for (size_t i = 0; i < dictionary->getCount(); ++i)
                pending.push_back(dictionary->getValue(i));
        }
        else if (object.isArray())
        {
            if (object.getArray()->getCount() + pending.size() > 4096)
                fail("共有字体の参照が処理上限を超えています。");
            for (const auto& item : *object.getArray())
                pending.push_back(item);
        }
    }
    if (declaration.isReference())
        fonts[declaration.getReference()] = matched;
    return matched;
}
void RedactionResourceUsage::validateFormSelection(const PDFObject& form,
                                                   const QPainterPath& regions) const
{
    if (!forbiddenForm(form))
        return;
    const auto object = source.getObject(form);
    if (!object.isStream())
        fail("共有Formの内容を確認できません。");
    const auto hash = form.isReference() && hashes.contains(form.getReference())
                          ? hashes.at(form.getReference())
                          : redactionStreamFingerprint(source.getStorage(), object.getStream(),
                                                       decodedTotal, cancelled);
    if (!formRegionsByHash.contains(hash) || formRegionsByHash[hash] != regions)
        fail("同じForm内容への異なる範囲指定は安全に処理できません。候補は出力していません。");
}
bool RedactionResourceUsage::affectedResources(const PDFObject& resources, int depth) const
{
    stop();
    if (depth > 8 || ++resourceVisits > 1000000)
        fail("共有Formの階層が処理上限を超えています。");
    const auto dictionary = source.getDictionaryFromObject(resources);
    if (!dictionary)
        fail("共有資源の辞書を確認できません。");
    if (dictionary->getCount() > 4096)
        fail("共有資源の辞書が処理上限を超えています。");
    if (const auto fontDictionary = source.getDictionaryFromObject(dictionary->get("Font")))
        for (size_t i = 0; i < fontDictionary->getCount(); ++i)
            if (affectedFont(fontDictionary->getValue(i)))
                return true;
    if (const auto objects = source.getDictionaryFromObject(dictionary->get("XObject")))
        for (size_t i = 0; i < objects->getCount(); ++i)
        {
            const auto value = objects->getValue(i);
            if (forbiddenForm(value))
                return true;
            const auto object = source.getObject(value);
            if (!object.isStream())
                fail("共有XObjectの参照を確認できません。");
            const auto form = object.getStream()->getDictionary();
            if (source.getObject(form->get("Subtype")) == PDFObject::createName("Form"))
            {
                if (!form->hasKey("Resources"))
                    fail("共有Formの継承資源を確認できません。");
                if (affectedResources(form->get("Resources"), depth + 1))
                    return true;
            }
        }
    return false;
}
} // namespace tatsu
