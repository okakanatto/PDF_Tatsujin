#include "ocr_job.h"
#include <algorithm>

namespace tatsu
{
namespace
{
void writeJobFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
        fail("OCR作業ファイルを記録できません。" + file.errorString());
}
} // namespace
bool OcrJobResult::changed() const
{
    return std::any_of(pages.cbegin(), pages.cend(),
                       [](const auto& page) { return page.status == "処理済み"; });
}
std::unique_ptr<OcrJob> OcrJob::prepare(const Document& document, const OcrOptions& options,
                                        const QString& temporaryRoot)
{
    document.editable();
    if (!QStringList{"jpn+eng", "jpn", "eng"}.contains(options.language))
        fail("OCR言語が不正です。");
    auto job = std::unique_ptr<OcrJob>(new OcrJob);
    job->targets = parsePages(options.pages, document.pages());
    job->revision = document.revision;
    job->pageCount = document.pages();
    job->options = options;
    job->directory = privateTemporaryDirectory(temporaryRoot + "/pdf-tatsujin-job-XXXXXX");
    if (!job->directory->isValid())
        fail("OCR一時領域を作成できません。");
    job->lock = std::make_unique<QLockFile>(job->filePath("job.lock"));
    job->lock->setStaleLockTime(0);
    if (!job->lock->tryLock())
        fail("OCR一時領域を確保できません。");
    // Publish ownership only after taking the lock so startup cleanup cannot
    // mistake a newly prepared job for an abandoned one.
    writeJobFile(job->filePath(".tatsujin-owner"), "PDFTatsujin job v1");
    writeCandidate(document.pdf(), job->filePath("input.pdf"));
    writeJobFile(
        job->filePath("options.json"),
        QJsonDocument(QJsonObject{{"language", options.language}, {"pages", options.pages}})
            .toJson());
    return job;
}
QString OcrJob::path() const
{
    return directory->path();
}
QString OcrJob::filePath(const QString& name) const
{
    return directory->filePath(name);
}
QStringList OcrJob::workerArguments() const
{
    return {"--ocr-worker", filePath("input.pdf"), filePath("result.pdf"), filePath("options.json"),
            filePath("report.json")};
}
OcrJobResult OcrJob::readResult(const Document& current) const
{
    if (current.revision != revision || current.pages() != pageCount)
        fail("文書の版が変わったためOCRを反映しませんでした。");
    QFile file(filePath("report.json"));
    if (!file.open(QIODevice::ReadOnly))
        fail("OCR結果の報告を読めません。開始前の変更を保持しました。");
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(file.readAll(), &error);
    if (file.error() != QFileDevice::NoError || error.error != QJsonParseError::NoError ||
        !json.isObject())
        fail("OCR結果の報告が不正です。開始前の変更を保持しました。");
    const auto report = json.object();
    if (report["language"].toString() != options.language || !report["pages"].isArray() ||
        report["pages"].toArray().size() != targets.size())
        fail("OCR結果の対象が一致しません。開始前の変更を保持しました。");
    OcrJobResult result;
    const auto rows = report["pages"].toArray();
    const QStringList statuses{"処理済み", "既存OCR保持", "文字未検出", "処理不要（既存文字）"};
    for (int i = 0; i < targets.size(); ++i)
    {
        const auto row = rows[i].toObject();
        const auto page = row["page"];
        const auto status = row["status"].toString();
        if (!rows[i].isObject() || !page.isDouble() || page.toDouble() != targets[i] + 1 ||
            !statuses.contains(status))
            fail("OCR結果のページ報告が不正です。開始前の変更を保持しました。");
        result.pages.append({targets[i] + 1, status});
    }
    result.document = readPdf(filePath("result.pdf"));
    if (int(result.document.getCatalog()->getPageCount()) != pageCount)
        fail("OCR結果のページ数が一致しません。");
    return result;
}
} // namespace tatsu
