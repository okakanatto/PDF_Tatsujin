#include "preview_raster.h"
#include "pdfannotation.h"
#include "pdfcms.h"
#include "pdffont.h"
#include "pdfpainter.h"
#include "pdfrenderer.h"

namespace tatsu
{
QImage renderPreview(PDFDocument& document, int page, double scale)
{
    const auto source = document.getCatalog()->getPage(page);
    const auto size = (pageSize(source) * scale).toSize();
    QImage image(size, QImage::Format_RGB32);
    if (image.isNull())
        fail("ページ一覧の描画メモリが不足しています。");
    image.fill(Qt::white);
    PDFFontCache fonts{128, 128};
    PDFCMSManager manager(nullptr);
    const PDFModifiedDocument modified(&document, nullptr);
    fonts.setDocument(modified);
    const auto cms = manager.getCurrentCMS();
    const auto features = PDFRenderer::getDefaultFeatures();
    PDFRenderer renderer(&document, &fonts, cms.data(), nullptr, features, {});
    PDFPrecompiledPage compiled;
    renderer.compile(&compiled, page);
    auto errors = compiled.getErrors();
    QPainter painter(&image);
    const auto matrix = pageMatrix(source, scale);
    compiled.draw(&painter, source->getCropBox(), matrix, features, 1.0);
    PDFAnnotationManager annotations(&fonts, &manager, nullptr, {}, features,
                                     PDFAnnotationManager::Target::View, nullptr);
    annotations.setDocument(modified);
    PDFTextLayoutCache text([&](PDFInteger number) { return textLayout(document, int(number)); });
    PDFTextLayoutGetter getter(&text, page);
    PDFColorConvertor color;
    annotations.drawPage(&painter, page, &compiled, getter, matrix, color, errors);
    painter.end();
    return image;
}
} // namespace tatsu
