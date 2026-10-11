#pragma once
#include "document.h"
#include <functional>

namespace pdf
{
class PDFDocumentBuilder;
}

namespace tatsu
{
struct ContentSpan
{
    qsizetype begin = 0, end = 0;
};
struct ContentInvocationOwner
{
    pdf::PDFObjectReference reference;
    pdf::PDFDictionary attributes, resources;
    QByteArray bytes;
    int parent = -1;
    ContentSpan parentSpan;
};
// Clone only the selected invocation's ancestors; shared siblings stay intact.
void spliceContentInvocation(pdf::PDFDocumentBuilder& builder, const pdf::PDFDocument& snapshot,
                             QVector<ContentInvocationOwner> owners, int selected, ContentSpan span,
                             QByteArray replacement, const std::function<void()>& checkCancelled);
} // namespace tatsu
