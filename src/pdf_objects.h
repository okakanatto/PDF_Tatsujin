#pragma once
#include "pdfobject.h"
#include <QRectF>

namespace tatsu::detail
{
using namespace pdf;
inline PDFObject dictObject(PDFDictionary d)
{
    return PDFObject::createDictionary(std::make_shared<PDFDictionary>(std::move(d)));
}
inline PDFObject arrObject(std::vector<PDFObject> a)
{
    return PDFObject::createArray(std::make_shared<PDFArray>(std::move(a)));
}
inline void set(PDFDictionary& d, const char* k, PDFObject v)
{
    d.setEntry(PDFInplaceOrMemoryString(k), std::move(v));
}
inline PDFObject number(double n)
{
    return PDFObject::createReal(n);
}
inline PDFObject rectObject(QRectF r)
{
    return arrObject({number(r.left()), number(r.top()), number(r.right()), number(r.bottom())});
}
inline PDFObject streamObject(PDFDictionary d, QByteArray b)
{
    set(d, "Length", PDFObject::createInteger(b.size()));
    return PDFObject::createStream(std::make_shared<PDFStream>(std::move(d), std::move(b)));
}
} // namespace tatsu::detail
