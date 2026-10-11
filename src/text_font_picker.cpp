#include "text_font_picker.h"
#include "document.h"
#include "text_font.h"

namespace tatsu
{
TextFontPicker::TextFontPicker(QWidget* parent) : QComboBox(parent)
{
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(12);
    setAccessibleName("挿入する文字の書体");
    for (const auto& family : textFontFamilies())
    {
        addItem(family, family);
        setItemData(count() - 1, QFont(family, 10), Qt::FontRole);
    }
    setToolTip("このPCで利用でき、PDFへ埋め込める書体です。"
               "日本語を含む場合は日本語対応の書体を選んでください。");
}
QString TextFontPicker::family() const
{
    return currentData().toString();
}
void TextFontPicker::setFamily(const QString& name)
{
    const auto family = name.isEmpty() ? signatureFont() : name;
    int index = findData(family);
    if (index < 0)
    {
        // Preserve the saved choice; silently replacing it would change the PDF.
        addItem(family + "（このPCでは利用できません）", family);
        index = count() - 1;
    }
    setCurrentIndex(index);
}
} // namespace tatsu
