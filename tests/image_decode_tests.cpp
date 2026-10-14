#include "image_decode_tests.h"
#include "device_image_decode.h"
#include "document.h"
#include "pdfoperationcontrol.h"
#include <algorithm>

namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        tatsu::fail(message);
}
// A derived CMS uses the untouched general conversion with the same upstream
// floating-point mapping, providing a reference independent of the byte path.
class ReferenceCMS : public pdf::PDFCMSGeneric
{
};
class InvertedCMS : public pdf::PDFCMSGeneric
{
public:
    bool fillRGBBufferFromDeviceRGB(const std::vector<float>& colors, pdf::RenderingIntent intent,
                                    unsigned char* output,
                                    pdf::PDFRenderErrorReporter* reporter) const override
    {
        PDFCMSGeneric::fillRGBBufferFromDeviceRGB(colors, intent, output, reporter);
        for (size_t i = 0; i < colors.size(); ++i)
            output[i] = 255 - output[i];
        return true;
    }
};
class Cancelled : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override
    {
        return true;
    }
};
pdf::PDFImageData
imageData(unsigned channels, unsigned bits = 8,
          pdf::PDFImageData::MaskingType mask = pdf::PDFImageData::MaskingType::None,
          std::vector<pdf::PDFReal> decode = {})
{
    const unsigned width = 257, height = 5, stride = width * channels * (bits / 8) + 3;
    QByteArray bytes(qsizetype(stride) * height, Qt::Uninitialized);
    for (qsizetype i = 0; i < bytes.size(); ++i)
        bytes[i] = char((i * 73 + i / 256) % 256);
    return pdf::PDFImageData(channels, bits, width, height, stride, mask, bytes, {},
                             std::move(decode), {});
}
} // namespace

QJsonObject testDeviceImageDecode(const QString& fixtures)
{
    pdf::PDFCMSGeneric cms;
    ReferenceCMS reference;
    tatsu::Document document;
    document.open(fixtures + "/D01.pdf");
    auto factory = [&](const char* name)
    {
        return pdf::PDFAbstractColorSpace::createColorSpace(nullptr, &document.pdf(),
                                                            pdf::PDFObject::createName(name));
    };
    const auto rgb = factory("DeviceRGB"), gray = factory("DeviceGray"),
               cmyk = factory("DeviceCMYK");
    QJsonArray cases;
    for (const bool isRGB : {false, true})
    {
        const pdf::PDFAbstractColorSpace& space = isRGB ? *rgb : *gray;
        const unsigned channels = isRGB ? 3 : 1;
        for (const bool explicitDecode : {false, true})
        {
            std::vector<pdf::PDFReal> decode;
            if (explicitDecode)
                for (unsigned i = 0; i < channels; ++i)
                    decode.insert(decode.end(), {0, 1});
            auto data = imageData(channels, 8, pdf::PDFImageData::MaskingType::None, decode);
            const auto fast = tatsu::tryDecodeDeviceImage(space, data, &cms, nullptr);
            const auto original =
                space.getImage(data, pdf::PDFImageData(), &reference,
                               pdf::RenderingIntent::RelativeColorimetric, nullptr, nullptr);
            check(fast && *fast == original, "Device image differs from original conversion");
            cases.append(QJsonObject{{"space", isRGB ? "DeviceRGB" : "DeviceGray"},
                                     {"explicit_identity_Decode", explicitDecode},
                                     {"all_sample_bytes_and_padding", "PASS"}});
        }
    }
    auto data = imageData(3);
    check(!tatsu::tryDecodeDeviceImage(*rgb, data, &reference, nullptr), "Derived CMS bypassed");
    InvertedCMS inverted;
    check(!tatsu::tryDecodeDeviceImage(*rgb, data, &inverted, nullptr), "Custom CMS bypassed");
    auto invertedImage =
        rgb->getImage(data, pdf::PDFImageData(), &inverted,
                      pdf::RenderingIntent::RelativeColorimetric, nullptr, nullptr);
    check(invertedImage.pixelColor(0, 0).red() == 255 - uchar(data.getData()[0]),
          "General custom CMS conversion was not used");
    for (const auto mask :
         {pdf::PDFImageData::MaskingType::SoftMask, pdf::PDFImageData::MaskingType::ColorKeyMasking,
          pdf::PDFImageData::MaskingType::ImageMask})
        check(!tatsu::tryDecodeDeviceImage(*rgb, imageData(3, 8, mask), &cms, nullptr),
              "Masked image bypassed");
    check(!tatsu::tryDecodeDeviceImage(*rgb, imageData(3, 16), &cms, nullptr),
          "16-bit image bypassed");
    check(!tatsu::tryDecodeDeviceImage(*cmyk, imageData(4), &cms, nullptr), "CMYK bypassed");
    auto inverse = imageData(3, 8, pdf::PDFImageData::MaskingType::None, {1, 0, 1, 0, 1, 0});
    check(!tatsu::tryDecodeDeviceImage(*rgb, inverse, &cms, nullptr),
          "Non-identity Decode bypassed");
    auto invalidDecode = imageData(3, 8, pdf::PDFImageData::MaskingType::None, {0, 1});
    check(!tatsu::tryDecodeDeviceImage(*rgb, invalidDecode, &cms, nullptr),
          "Invalid Decode bypassed");
    const pdf::PDFImageData shortData(3, 8, 257, 5, data.getStride(),
                                      pdf::PDFImageData::MaskingType::None, QByteArray(7, 'x'), {},
                                      {}, {});
    check(!tatsu::tryDecodeDeviceImage(*rgb, shortData, &cms, nullptr),
          "Truncated buffer bypassed");
    const pdf::PDFImageData shortStride(3, 8, 257, 5, 2, pdf::PDFImageData::MaskingType::None,
                                        data.getData(), {}, {}, {});
    check(!tatsu::tryDecodeDeviceImage(*rgb, shortStride, &cms, nullptr),
          "Invalid stride bypassed");
    Cancelled cancelled;
    const auto interrupted = tatsu::tryDecodeDeviceImage(*rgb, data, &cms, &cancelled);
    check(interrupted && interrupted->pixelColor(0, 0) == Qt::white,
          "Cancelled conversion continued reading samples");
    return {{"pixel_exact_reference", cases},
            {"fallbacks_preserved", true},
            {"cancelled_conversion", true}};
}
