#pragma once
#include "pdfcms.h"
#include "pdfcolorspaces.h"
#include <optional>

namespace tatsu
{
// Compiled into the same DLL as the upstream image conversion. Returning no
// value means the caller must use the original general conversion path.
PDF4QTLIBCORESHARED_EXPORT std::optional<QImage>
tryDecodeDeviceImage(const pdf::PDFAbstractColorSpace& colorSpace, const pdf::PDFImageData& data,
                     const pdf::PDFCMS* cms, const pdf::PDFOperationControl* control);
} // namespace tatsu
