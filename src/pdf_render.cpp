#include "document.h"
#include "pdfannotation.h"
#include "pdfcms.h"
#include "pdffont.h"
#include "pdfpainter.h"
#include "pdfrenderer.h"
#include "pdfsecurityhandler.h"
#include "pdftextlayoutgenerator.h"

namespace tatsu
{
using namespace pdf;
QSizeF pageSize(const PDFPage* p, bool rotate)
{
    auto s = p->getCropBox().size() * p->getUserUnit();
    return rotate ? PDFPage::getRotatedSize(s, p->getPageRotation()) : s;
}
QTransform pageMatrix(const PDFPage* p, double scale, bool rotate)
{
    const auto box = p->getCropBox();
    const double factor = scale * p->getUserUnit();
    // PDF coordinates have an upward y axis. Map the rotated CropBox directly
    // to the physical page, preserving the same scale on both axes.
    switch (rotate ? p->getPageRotation() : PageRotation::None)
    {
    case PageRotation::None:
        return {factor, 0, 0, -factor, -box.left() * factor, box.bottom() * factor};
    case PageRotation::Rotate90:
        return {0, factor, factor, 0, -box.top() * factor, -box.left() * factor};
    case PageRotation::Rotate180:
        return {-factor, 0, 0, factor, box.right() * factor, -box.top() * factor};
    case PageRotation::Rotate270:
        return {0, -factor, -factor, 0, box.bottom() * factor, box.right() * factor};
    }
    fail("ページの回転が不正です。");
}
struct RenderContext
{
    PDFFontCache fonts{128, 128};
    PDFCMSManager cms{nullptr};
    explicit RenderContext(PDFDocument& d)
    {
        fonts.setDocument(PDFModifiedDocument(&d, nullptr));
    }
};
PDFTextLayout textLayout(PDFDocument& d, int page, const QTransform& m)
{
    RenderContext c(d);
    auto cms = c.cms.getCurrentCMS();
    PDFTextLayoutGenerator gen(PDFRenderer::getDefaultFeatures(), d.getCatalog()->getPage(page), &d,
                               &c.fonts, cms.data(), nullptr, m, {});
    gen.processContents();
    return gen.createTextLayout();
}
QString pageText(PDFDocument& d, int p)
{
    auto layout = textLayout(d, p);
    QString t;
    for (auto& f : PDFTextFlow::createTextFlows(layout, PDFTextFlow::AddLineBreaks, p))
        t += f.getText();
    return t;
}
QImage renderPage(PDFDocument& d, int page, double scale, bool annotations, bool rotate,
                  RenderPurpose purpose)
{
    auto p = d.getCatalog()->getPage(page);
    auto size = pageSize(p, rotate) * scale;
    if (size.width() * size.height() > 100000000)
        fail("ページの描画サイズが大きすぎます。");
    QImage out(size.toSize(), QImage::Format_RGB32);
    if (out.isNull())
        fail("描画メモリが不足しています。");
    out.fill(Qt::white);
    RenderContext c(d);
    auto cms = c.cms.getCurrentCMS();
    auto features = PDFRenderer::getDefaultFeatures();
    PDFRenderer r(&d, &c.fonts, cms.data(), nullptr, features, {});
    QPainter paint(&out);
    auto m = pageMatrix(p, scale, rotate);
    auto errors = r.render(&paint, m, page);
    if (annotations)
    {
        PDFPrecompiledPage compiled;
        r.compile(&compiled, page);
        const auto target = purpose == RenderPurpose::Print ? PDFAnnotationManager::Target::Print
                                                            : PDFAnnotationManager::Target::View;
        PDFAnnotationManager a(&c.fonts, &c.cms, nullptr, {}, features, target, nullptr);
        a.setDocument(PDFModifiedDocument(&d, nullptr));
        PDFTextLayoutCache cache([&](PDFInteger i) { return textLayout(d, int(i)); });
        PDFTextLayoutGetter getter(&cache, page);
        PDFColorConvertor color;
        a.drawPage(&paint, page, &compiled, getter, m, color, errors);
    }
    paint.end();
    return out;
}
void printDocument(PDFDocument& doc, QPrinter& printer, int currentPage)
{
    auto security = doc.getStorage().getSecurityHandler();
    bool high = security->isAllowed(PDFSecurityHandler::Permission::PrintHighResolution);
    if (!high && !security->isAllowed(PDFSecurityHandler::Permission::PrintLowResolution))
        fail("この文書では印刷が許可されていません。");
    if (!printer.isValid())
        fail("利用できるプリンターを選択してください。");
    const int count = int(doc.getCatalog()->getPageCount());
    int first = 0, last = count - 1;
    if (printer.printRange() == QPrinter::PageRange)
    {
        first = printer.fromPage() - 1;
        last = printer.toPage() - 1;
    }
    else if (printer.printRange() == QPrinter::CurrentPage)
        first = last = currentPage;
    else if (printer.printRange() == QPrinter::Selection)
        fail("選択範囲の印刷には対応していません。ページ範囲を指定してください。");
    if (first < 0 || last < first || last >= count)
        fail("印刷するページ範囲を確認してください。");
    QList<int> selectedPages;
    for (int i = first; i <= last; ++i)
        selectedPages.append(i);
    if (printer.pageOrder() == QPrinter::LastPageFirst)
        std::reverse(selectedPages.begin(), selectedPages.end());
    QPainter painter;
    bool started = false;
    for (const int i : selectedPages)
    {
        auto dims = pageSize(doc.getCatalog()->getPage(i));
        printer.setPageSize(QPageSize(dims * 25.4 / 72, QPageSize::Millimeter));
        printer.setPageMargins(QMarginsF(0, 0, 0, 0));
        printer.setFullPage(true);
        if (!started)
        {
            if (!painter.begin(&printer))
                fail("印刷を開始できません。");
            started = true;
        }
        else if (!printer.newPage())
            fail("次のページを印刷できません。");
        auto image =
            renderPage(doc, i, (high ? 300.0 : 150.0) / 72, true, true, RenderPurpose::Print);
        painter.drawImage(printer.pageRect(QPrinter::DevicePixel), image);
    }
    if (!painter.end() || printer.printerState() == QPrinter::Error)
        fail("印刷を完了できませんでした。出力先とプリンターの状態を確認してください。");
}
} // namespace tatsu
