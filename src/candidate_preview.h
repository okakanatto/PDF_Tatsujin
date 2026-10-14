#pragma once
#include "document.h"
#include <QThread>
#include <QTimer>
#include <atomic>
#include <functional>

namespace tatsu
{
struct CandidatePreviewResult
{
    PDFDocument document;
    QImage image;
    QSizeF dimensions;
    QVector<QRectF> bounds;
    QString error;
};
// Owns one worker; coalesces drafts and only delivers the most recent result.
class CandidatePreview final : public QObject
{
public:
    using Cancel = std::function<bool()>;
    using Prepare = std::function<PDFDocument(const Cancel&)>;
    using Inspect = std::function<QVector<QRectF>(const PDFDocument&)>;
    explicit CandidatePreview(QObject* parent = nullptr);
    ~CandidatePreview() override;
    std::function<void(CandidatePreviewResult)> ready;
    void request(Prepare prepare, int page, double longestEdge, Inspect inspect = {});
    void cancel(std::function<void()> finished);
    bool running() const
    {
        return job != nullptr;
    }

private:
    QTimer timer;
    QThread* job = nullptr;
    Prepare pending;
    Inspect inspect;
    int page = 0;
    double pixels = 1000;
    quint64 generation = 0;
    bool closing = false;
    std::function<void()> cancelled;
    std::shared_ptr<std::atomic_bool> stopped = std::make_shared<std::atomic_bool>(false);
    void start();
};
} // namespace tatsu
