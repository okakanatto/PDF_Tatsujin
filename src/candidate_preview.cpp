#include "candidate_preview.h"

namespace tatsu
{
CandidatePreview::CandidatePreview(QObject* parent) : QObject(parent)
{
    timer.setSingleShot(true);
    timer.setInterval(120);
    connect(&timer, &QTimer::timeout, this, [this] { start(); });
}
CandidatePreview::~CandidatePreview()
{
    stopped->store(true);
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
        delete job;
    }
}
void CandidatePreview::request(Prepare prepare, int number, double longestEdge)
{
    if (closing)
        return;
    pending = std::move(prepare);
    page = number;
    pixels = qBound(500.0, longestEdge, 3000.0);
    ++generation;
    timer.start();
}
void CandidatePreview::cancel(std::function<void()> finished)
{
    if (closing)
        return;
    closing = true;
    stopped->store(true);
    timer.stop();
    pending = {};
    cancelled = std::move(finished);
    if (!job && cancelled)
        cancelled();
}
void CandidatePreview::start()
{
    if (job || closing || !pending)
        return;
    const auto token = generation;
    const auto prepare = pending;
    const auto number = page;
    const auto edge = pixels;
    const auto stop = stopped;
    auto result = std::make_shared<CandidatePreviewResult>();
    job = QThread::create(
        [prepare, number, edge, stop, result]
        {
            try
            {
                result->document = prepare([stop] { return stop->load(); });
                if (stop->load())
                    return;
                result->dimensions = pageSize(result->document.getCatalog()->getPage(number));
                result->image = renderPage(
                    result->document, number,
                    edge / qMax(result->dimensions.width(), result->dimensions.height()));
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "変更を確認できません。元の文書は保持しています。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, token, result]
            {
                job = nullptr;
                launched->deleteLater();
                if (closing)
                {
                    if (cancelled)
                        cancelled();
                    return;
                }
                if (token != generation)
                {
                    timer.start();
                    return;
                }
                if (ready)
                    ready(std::move(*result));
            });
    launched->start();
}
} // namespace tatsu
