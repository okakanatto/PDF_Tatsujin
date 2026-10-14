#pragma once
#include "content_invocation_edit.h"

namespace tatsu
{
struct TrackedContentOwner : ContentInvocationOwner
{
    QVector<ContentSpan> drawings;
    QVector<QPolygonF> clips;
    int cursor = 0;
};
struct XObjectDrawing
{
    QByteArray name, subtype;
    pdf::PDFObjectReference reference;
    pdf::PDFDictionary attributes;
    int owner = 0, occurrence = 0;
    ContentSpan span;
};
class FormContentInvocations
{
public:
    FormContentInvocations(const pdf::PDFDocument& document, int page,
                           std::function<void()> checkCancelled = {});
    XObjectDrawing beginDrawing(const QByteArray& name, const pdf::PDFDictionary* xobjects,
                                const QTransform& matrix);
    void endDrawing();
    bool finished() const;
    int currentOwner() const
    {
        return active.last();
    }
    int depth() const
    {
        return active.size() - 1;
    }
    QVector<TrackedContentOwner> owners;
    QTransform physicalMatrix;

private:
    const pdf::PDFDocument& document;
    std::function<void()> checkCancelled;
    QVector<int> active{0};
    QVector<bool> pushed;
    int count = 0;
    qsizetype decodedBytes = 0;
};
bool contentBoundsContain(const QVector<QPolygonF>& clips, const QPolygonF& shape);
} // namespace tatsu
