#pragma once
#include "document.h"
#include "ocr_job.h"
#include <functional>

namespace tatsu
{
enum class BatchOperation
{
    Ocr,
    Optimize
};
enum class BatchStatus
{
    Pending,
    Running,
    Saved,
    Unneeded,
    Failed,
    Cancelled,
    NotProcessed
};
struct BatchInput
{
    QString source, destination;
    QByteArray hash;
    qint64 bytes = 0;
};
struct BatchPlan
{
    QVector<BatchInput> inputs;
    BatchOperation operation = BatchOperation::Ocr;
    QString language = "jpn+eng";
    QString outputDirectory;
};
struct BatchItemResult
{
    BatchStatus status = BatchStatus::Pending;
    QString source, destination, message;
    QVector<OcrPageResult> pages;
    QByteArray outputHash;
};
using BatchProgress = std::function<void(int, const BatchItemResult&)>;
QString batchStatusText(BatchStatus status);
BatchPlan prepareBatch(const QStringList& inputs, const QString& outputDirectory,
                       BatchOperation operation, const QString& language = "jpn+eng",
                       const std::function<bool()>& cancelled = {});
QVector<BatchItemResult> processBatch(const BatchPlan& plan,
                                      const std::function<bool()>& cancelled = {},
                                      const BatchProgress& progress = {});
} // namespace tatsu
