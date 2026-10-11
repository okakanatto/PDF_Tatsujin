#pragma once
#include "document.h"
#include "pdfdocumentbuilder.h"
#include <functional>
namespace tatsu
{
PDFObjectReference painterAppearance(PDFDocumentBuilder& builder, QSizeF dimensions,
                                     const std::function<void(QPainter*)>& draw,
                                     const QString& text = {}, const QRawFont& font = {});
} // namespace tatsu
