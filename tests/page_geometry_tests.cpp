#include "page_geometry_tests.h"
#include "document.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "preview_raster.h"
#include <cstring>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
QPointF visualPoint(const PDFPage* page, QPointF point)
{
    const auto box = page->getCropBox();
    QPointF visual;
    switch (page->getPageRotation())
    {
    case PageRotation::None:
        visual = {point.x() - box.left(), box.bottom() - point.y()};
        break;
    case PageRotation::Rotate90:
        visual = {point.y() - box.top(), point.x() - box.left()};
        break;
    case PageRotation::Rotate180:
        visual = {box.right() - point.x(), point.y() - box.top()};
        break;
    case PageRotation::Rotate270:
        visual = {box.bottom() - point.y(), box.right() - point.x()};
        break;
    }
    return visual * page->getUserUnit();
}
bool equalPixels(QImage first, QImage second)
{
    if (first.size() != second.size())
        return false;
    first = first.convertToFormat(QImage::Format_RGBA8888);
    second = second.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < first.height(); ++y)
        if (memcmp(first.constScanLine(y), second.constScanLine(y), size_t(first.width()) * 4))
            return false;
    return true;
}
} // namespace
QJsonObject testPageAxes(const QString& fixtures)
{
    const auto document = readPdf(fixtures + "/D02.pdf");
    double error = 0;
    int conditions = 0;
    for (int i = 0; i < 4; ++i)
    {
        const auto page = document.getCatalog()->getPage(i);
        const auto box = page->getCropBox();
        for (double scale : {.5, 1., 1.5, 2.})
        {
            ++conditions;
            const auto matrix = pageMatrix(page, scale);
            const auto bounds = matrix.mapRect(box);
            const auto dimensions = pageSize(page) * scale;
            error = std::max(error, QLineF(bounds.topLeft(), QPointF()).length());
            error = std::max(error, QLineF(bounds.bottomRight(),
                                           QPointF(dimensions.width(), dimensions.height()))
                                        .length());
            for (const auto& point :
                 {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight(), box.center()})
                error = std::max(
                    error, QLineF(matrix.map(point), visualPoint(page, point) * scale).length());
            check(error <= .000001,
                  QString("Correct physical page axes: page %1 scale %2 error %3 pt")
                      .arg(i + 1)
                      .arg(scale)
                      .arg(error));
        }
    }
    return {{"conditions", conditions}, {"max_error_pt", error}, {"limit_pt", .000001}};
}
QJsonObject testPageGeometryRender(const QString& fixtures, const QString& output)
{
    const auto original = readPdf(fixtures + "/D02.pdf");
    PDFDocumentBuilder builder;
    builder.createDocument();
    using namespace detail;
    const QVector<QColor> colors{QColor(224, 32, 64), QColor(32, 176, 64), QColor(32, 64, 224),
                                 QColor(224, 176, 32)};
    QJsonArray manifest;
    for (int i = 0; i < 4; ++i)
    {
        const auto source = original.getCatalog()->getPage(i);
        const auto page = builder.appendPage(source->getMediaBox());
        builder.setPageCropBox(page, source->getCropBox());
        builder.setPageRotation(page, source->getPageRotation());
        builder.setPageUserUnit(page, source->getUserUnit());
        const auto box = source->getCropBox();
        const double inset = 18 / source->getUserUnit(), side = 12 / source->getUserUnit();
        const QVector<QPointF> points{{box.left() + inset, box.top() + inset},
                                      {box.right() - inset - side, box.top() + inset},
                                      {box.left() + inset, box.bottom() - inset - side},
                                      {box.right() - inset - side, box.bottom() - inset - side}};
        QByteArray contents;
        QJsonArray markers;
        for (int j = 0; j < 4; ++j)
        {
            const auto color = colors[j];
            const auto point = points[j];
            contents += QString("q %1 %2 %3 rg %4 %5 %6 %6 re f Q\n")
                            .arg(color.redF(), 0, 'g', 14)
                            .arg(color.greenF(), 0, 'g', 14)
                            .arg(color.blueF(), 0, 'g', 14)
                            .arg(point.x(), 0, 'g', 14)
                            .arg(point.y(), 0, 'g', 14)
                            .arg(side, 0, 'g', 14)
                            .toLatin1();
            markers.append(
                QJsonObject{{"color", QJsonArray{color.red(), color.green(), color.blue()}},
                            {"PDF_rectangle", QJsonArray{point.x(), point.y(), side, side}}});
        }
        auto dictionary = *builder.getObjectByReference(page).getDictionary();
        set(dictionary, "Contents",
            PDFObject::createReference(builder.addObject(streamObject({}, contents))));
        builder.setObject(page, dictObject(dictionary));
        manifest.append(QJsonObject{{"page", i + 1}, {"scale", 1.5}, {"markers", markers}});
    }
    auto document = builder.build();
    writeCandidate(document, output + "/geometry-markers.pdf");
    for (int i = 0; i < 4; ++i)
    {
        const auto render = renderPage(document, i, 1.5);
        const auto preview = renderPreview(document, i, 1.5);
        check(equalPixels(render, preview), "Rotated page preview matches the common renderer");
        check(render.save(output + QString("/geometry-render-%1.png").arg(i + 1)), "Save render");
        check(preview.save(output + QString("/geometry-preview-%1.png").arg(i + 1)),
              "Save preview");
    }
    QFile file(output + "/geometry-markers.json");
    check(file.open(QIODevice::WriteOnly), "Write geometry expectations");
    file.write(QJsonDocument(manifest).toJson());
    return {{"pages", 4}, {"colored_markers", 16}, {"preview_pixels_equal", true}};
}
} // namespace tatsu
