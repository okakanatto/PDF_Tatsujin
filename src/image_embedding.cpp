#include "image_embedding.h"
#include "document.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfimage.h"

namespace tatsu
{
pdf::PDFObjectReference embedImage(pdf::PDFDocumentBuilder& builder, const QImage& image)
{
    using namespace pdf;
    using namespace detail;
    if (image.isNull())
        fail("空の画像を埋め込めません。");
    PDFImage::ImageEncodeOptions options;
    options.compression = PDFImage::ImageCompression::Flate;
    options.colorMode = PDFImage::ImageColorMode::Color;
    options.alphaHandling = PDFImage::AlphaHandling::DropAlphaPreserveColors;
    auto encoded = PDFImage::createStreamFromImage(image, options);
    auto dictionary = *encoded.getDictionary();
    if (image.hasAlphaChannel())
    {
        QImage alpha(image.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                alpha.scanLine(y)[x] = image.pixelColor(x, y).alpha();
        auto maskOptions = options;
        maskOptions.colorMode = PDFImage::ImageColorMode::Grayscale;
        auto mask = PDFImage::createStreamFromImage(alpha, maskOptions);
        auto reference = builder.addObject(
            PDFObject::createStream(std::make_shared<PDFStream>(std::move(mask))));
        set(dictionary, "SMask", PDFObject::createReference(reference));
    }
    return builder.addObject(streamObject(dictionary, *encoded.getContent()));
}
} // namespace tatsu
