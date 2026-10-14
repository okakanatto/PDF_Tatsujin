#include "page_crop.h"
#include "pdfdocumentbuilder.h"
#include <cmath>

namespace tatsu
{
QRectF croppedPageBox(const PDFPage* page, QMarginsF millimeters)
{
    if (!page)
        fail("ページがありません。");
    for (double value :
         {millimeters.left(), millimeters.top(), millimeters.right(), millimeters.bottom()})
        if (!std::isfinite(value) || value < 0 || value > 500)
            fail("除く長さは0〜500mmで指定してください。");
    const double factor = 72.0 / 25.4;
    const auto physical = pageSize(page);
    const auto remaining =
        QRectF(QPointF(), physical)
            .marginsRemoved({millimeters.left() * factor, millimeters.top() * factor,
                             millimeters.right() * factor, millimeters.bottom() * factor});
    if (!std::isfinite(remaining.width()) || !std::isfinite(remaining.height()) ||
        remaining.width() < factor || remaining.height() < factor)
        fail("変更後の用紙の幅・高さは1mm以上必要です。");
    bool invertible = false;
    const auto inverse = pageMatrix(page).inverted(&invertible);
    if (!invertible)
        fail("ページの座標変換が不正です。");
    return inverse.mapRect(remaining);
}
PDFDocument cropPages(const PDFDocument& document, const QVector<int>& pages, QMarginsF millimeters)
{
    if (!document.getCatalog() || !document.getCatalog()->getPageCount() ||
        !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    if (pages.isEmpty())
        fail("調整するページを選択してください。");
    QSet<int> seen;
    QVector<QRectF> boxes;
    for (int number : pages)
    {
        if (number < 0 || number >= int(document.getCatalog()->getPageCount()) ||
            seen.contains(number))
            fail("ページ範囲が不正か重複しています。");
        seen.insert(number);
        boxes.append(croppedPageBox(document.getCatalog()->getPage(number), millimeters));
    }
    if (millimeters.isNull())
        return document;
    PDFDocumentBuilder builder(&document);
    for (int i = 0; i < pages.size(); ++i)
        builder.setPageCropBox(document.getCatalog()->getPage(pages[i])->getPageReference(),
                               boxes[i]);
    return builder.build();
}
} // namespace tatsu
