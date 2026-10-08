#pragma once
#include "document.h"
#include "private_temp.h"
#include <QLockFile>

namespace tatsu
{
struct OcrOptions
{
    QString language;
    QString pages;
};
struct OcrPageResult
{
    int page;
    QString status;
};
struct OcrJobResult
{
    PDFDocument document;
    QVector<OcrPageResult> pages;
    bool changed() const;
};

// Owns one immutable input, its private files and parent lock. Reading a result
// validates the complete worker protocol; only the caller can commit Document.
class OcrJob
{
public:
    static std::unique_ptr<OcrJob> prepare(const Document& document, const OcrOptions& options,
                                           const QString& temporaryRoot = QDir::tempPath());
    QString path() const;
    QString filePath(const QString& name) const;
    QStringList workerArguments() const;
    OcrJobResult readResult(const Document& current) const;

private:
    OcrJob() = default;
    // Reverse destruction order releases the lock before deleting its directory.
    std::unique_ptr<PrivateTemporaryDirectory> directory;
    std::unique_ptr<QLockFile> lock;
    quint64 revision = 0;
    int pageCount = 0;
    OcrOptions options;
    QVector<int> targets;
};
} // namespace tatsu
