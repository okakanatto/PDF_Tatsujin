#pragma once
#include "pdfobject.h"
#include <QImage>

namespace pdf
{
class PDFDocumentBuilder;
}
namespace tatsu
{
pdf::PDFObjectReference embedImage(pdf::PDFDocumentBuilder& builder, const QImage& image);
}
