#include "redaction_standard_font.h"
#include "document.h"
#include "pdf_objects.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace tatsu
{
namespace
{
using namespace pdf;
template <size_t N>
bool unknownKey(const PDFDictionary& dictionary, const std::array<QByteArrayView, N>& allowed)
{
    for (size_t index = 0; index < dictionary.getCount(); ++index)
    {
        const auto key = dictionary.getKey(index).getString();
        if (std::none_of(allowed.begin(), allowed.end(),
                         [&](QByteArrayView known) { return key == known; }))
            return true;
    }
    return false;
}
} // namespace
void mergeRemovedDirectRedactionFonts(QVector<PDFObject>& target,
                                      const QVector<PDFObject>& declarations,
                                      const std::function<bool()>& cancelled)
{
    for (const auto& declaration : declarations)
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
        if (!declaration.isDictionary())
            fail("直格納の字体宣言を確認できません。");
        if (!target.contains(declaration))
        {
            if (target.size() >= 4096)
                fail("直格納の共有字体が処理上限を超えています。候補は出力していません。");
            target.append(declaration);
        }
    }
}
std::optional<float> redactionStandardTextAdjustment(float from, float target, float size,
                                                     const std::function<bool()>& cancelled)
{
    if (!std::isfinite(from) || !std::isfinite(target) || !std::isfinite(size) || size <= 0)
        return {};
    // PDF readers parse TJ numbers and accumulate positions as floats. The
    // closest inverse can overshoot one ULP after multiplication and division.
    // Find a nearby operand that reproduces the source cursor exactly.
    float below = float((double(from) - double(target)) * 1000.0 / double(size));
    float above = below;
    auto matches = [&](float value)
    { return std::isfinite(value) && from - value * size / 1000 == target; };
    for (int step = 0; step <= 32; ++step)
    {
        if (cancelled && cancelled())
            fail("墨消しを中止しました。文書は変更していません。");
        if (matches(below))
            return below;
        if (matches(above))
            return above;
        below = std::nextafter(below, -std::numeric_limits<float>::infinity());
        above = std::nextafter(above, std::numeric_limits<float>::infinity());
    }
    return {};
}
bool requiresRedactionFontSchemaCleanup(const PDFDocument& source, const PDFDictionary& declaration)
{
    if (source.getObject(declaration.get("Subtype")) != PDFObject::createName("Type1"))
        return false;
    // Even obsolete Name and known descriptor string fields may carry private
    // data. Only the primitive fields of the evaluated profile are reusable.
    static const std::array<QByteArrayView, 4> fontKeys = {"Type", "Subtype", "BaseFont",
                                                           "Encoding"};
    return unknownKey(declaration, fontKeys);
}
std::optional<PDFDictionary> canonicalRedactionStandardFont(const PDFDocument& source,
                                                            const PDFObject& original)
{
    const auto declaration = source.getDictionaryFromObject(original);
    static const std::array<QByteArrayView, 12> faces = {
        "Helvetica",   "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
        "Times-Roman", "Times-Bold",     "Times-Italic",      "Times-BoldItalic",
        "Courier",     "Courier-Bold",   "Courier-Oblique",   "Courier-BoldOblique"};
    const auto base = declaration ? source.getObject(declaration->get("BaseFont")) : PDFObject();
    if (!declaration ||
        source.getObject(declaration->get("Subtype")) != PDFObject::createName("Type1") ||
        !base.isName() ||
        std::none_of(faces.begin(), faces.end(),
                     [&](QByteArrayView face) { return base.getString() == face; }))
        return {};
    for (const auto key : {"FontDescriptor", "ToUnicode", "Widths", "FirstChar", "LastChar"})
        if (declaration->hasKey(key))
            return {};
    const auto encoding = source.getObject(declaration->get("Encoding"));
    if (!encoding.isNull() && encoding != PDFObject::createName("StandardEncoding") &&
        encoding != PDFObject::createName("WinAnsiEncoding"))
        return {};
    const auto type = source.getObject(declaration->get("Type"));
    if (!type.isNull() && type != PDFObject::createName("Font"))
        return {};
    PDFDictionary clean;
    detail::set(clean, "Type", PDFObject::createName("Font"));
    detail::set(clean, "Subtype", PDFObject::createName("Type1"));
    detail::set(clean, "BaseFont", PDFObject(base));
    if (!encoding.isNull())
        detail::set(clean, "Encoding", PDFObject(encoding));
    return clean;
}
} // namespace tatsu
