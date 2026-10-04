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
    return PDFRenderer::createMediaBoxToDevicePointMatrix(
        p->getCropBox(), QRectF(QPointF(0, 0), pageSize(p, rotate) * scale),
        rotate ? p->getPageRotation() : PageRotation::None);
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
QImage renderPage(PDFDocument& d, int page, double scale, bool annotations, bool rotate)
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
        PDFAnnotationManager a(&c.fonts, &c.cms, nullptr, {}, features,
                               PDFAnnotationManager::Target::View, nullptr);
        a.setDocument(PDFModifiedDocument(&d, nullptr));
        PDFTextLayoutCache cache([&](PDFInteger i) { return textLayout(d, int(i)); });
        PDFTextLayoutGetter getter(&cache, page);
        PDFColorConvertor color;
        a.drawPage(&paint, page, &compiled, getter, m, color, errors);
    }
    paint.end();
    return out;
}
void printDocument(PDFDocument& doc, QPrinter& printer)
{
    auto security = doc.getStorage().getSecurityHandler();
    bool high = security->isAllowed(PDFSecurityHandler::Permission::PrintHighResolution);
    if (!high && !security->isAllowed(PDFSecurityHandler::Permission::PrintLowResolution))
        fail("この文書では印刷が許可されていません。");
    QPainter painter;
    for (int i = 0; i < int(doc.getCatalog()->getPageCount()); ++i)
    {
        auto dims = pageSize(doc.getCatalog()->getPage(i));
        printer.setPageSize(QPageSize(dims * 25.4 / 72, QPageSize::Millimeter));
        printer.setPageMargins(QMarginsF(0, 0, 0, 0));
        printer.setFullPage(true);
        if (i == 0)
        {
            if (!painter.begin(&printer))
                fail("印刷を開始できません。");
        }
        else if (!printer.newPage())
            fail("次のページを印刷できません。");
        auto image = renderPage(doc, i, (high ? 300.0 : 150.0) / 72);
        painter.drawImage(printer.pageRect(QPrinter::DevicePixel), image);
    }
    painter.end();
}
} // namespace tatsu
