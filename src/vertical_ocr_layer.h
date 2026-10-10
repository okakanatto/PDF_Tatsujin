#pragma once
#include "document.h"
namespace tesseract
{
class TessBaseAPI;
}
namespace tatsu
{
PDFDocument verticalOcrLayer(tesseract::TessBaseAPI& api, QSizeF points, const QImage& image);
}
