#pragma once
#include "pdfobject.h"
#include <QImage>

namespace pdf
{
class PDFDocumentBuilder;
}
namespace tatsu
{
enum class ImagePrediction
{
    Png,
    None
};
pdf::PDFObjectReference embedImage(pdf::PDFDocumentBuilder& builder, const QImage& image,
                                   ImagePrediction prediction = ImagePrediction::Png);
} // namespace tatsu
