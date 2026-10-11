#include "device_image_decode.h"
#include "pdfoperationcontrol.h"
#include <cstring>
#include <limits>
#include <typeinfo>

namespace tatsu
{
std::optional<QImage> tryDecodeDeviceImage(const pdf::PDFAbstractColorSpace& colorSpace,
                                           const pdf::PDFImageData& data, const pdf::PDFCMS* cms,
                                           const pdf::PDFOperationControl* control)
{
    // The pinned generic CMS maps 8-bit DeviceRGB/Gray samples back to exactly
    // the same bytes. Never bypass ICC, calibration, masks, or a derived CMS.
    const bool rgb = typeid(colorSpace) == typeid(pdf::PDFDeviceRGBColorSpace);
    const bool gray = typeid(colorSpace) == typeid(pdf::PDFDeviceGrayColorSpace);
    const auto channels = rgb ? 3u : 1u;
    if ((!rgb && !gray) || !cms || typeid(*cms) != typeid(pdf::PDFCMSGeneric) ||
        cms->getColorConvertor().isActive() || !data.isValid() ||
        data.getComponents() != channels || data.getBitsPerComponent() != 8 ||
        data.getMaskingType() != pdf::PDFImageData::MaskingType::None)
        return {};
    const auto& decode = data.getDecode();
    if (!decode.empty())
    {
        if (decode.size() != channels * 2)
            return {};
        for (size_t i = 0; i < decode.size(); ++i)
            if (decode[i] != (i % 2))
                return {};
    }
    const auto width = data.getWidth(), height = data.getHeight(), stride = data.getStride();
    const auto rowBytes = quint64(width) * channels;
    if (width > unsigned(std::numeric_limits<int>::max()) ||
        height > unsigned(std::numeric_limits<int>::max()) || stride < rowBytes ||
        quint64(height) * stride > quint64(data.getData().size()))
        return {};
    QImage image(int(width), int(height), QImage::Format_RGB888);
    if (image.isNull())
        return {};
    image.fill(Qt::white);
    for (unsigned row = 0; row < height; ++row)
    {
        if (pdf::PDFOperationControl::isOperationCancelled(control))
            break;
        const auto source =
            reinterpret_cast<const uchar*>(data.getData().constData()) + quint64(row) * stride;
        auto target = image.scanLine(int(row));
        if (rgb)
            std::memcpy(target, source, size_t(rowBytes));
        else
            for (unsigned column = 0; column < width; ++column)
            {
                const auto value = source[column];
                *target++ = value;
                *target++ = value;
                *target++ = value;
            }
    }
    return image;
}
} // namespace tatsu
