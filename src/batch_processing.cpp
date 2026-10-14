#include "batch_processing.h"
#include "ocr_language.h"
#include "pdf_optimization.h"
#include "pdfexception.h"
#include "save_candidate.h"
#include "windows_path.h"
#include "worker_channels.h"
#include <windows.h>

namespace tatsu
{
namespace
{
constexpr qint64 fileLimit = 256ll * 1024 * 1024;
bool stopping(const std::function<bool()>& cancelled)
{
    return cancelled && cancelled();
}
void stop(const std::function<bool()>& cancelled)
{
    if (stopping(cancelled))
        fail("処理を中止しました。入力ファイルは保持しています。");
}
void unchanged(const BatchInput& input)
{
    const QFileInfo file(input.source);
    if (!file.isFile() || file.size() != input.bytes || fileHash(input.source) != input.hash)
        fail("入力ファイルが更新されました。出力せず、次の入力へ進みます。");
}
OcrJobResult recognize(Document& document, const QString& language,
                       const std::function<bool()>& cancelled,
                       const std::function<void(QString)>& progress)
{
    auto job = OcrJob::prepare(document, {language, QString("1-%1").arg(document.pages())});
    QProcess process;
    WorkerChannels channels(process, job->path());
    process.start(QCoreApplication::applicationFilePath(), job->workerArguments());
    if (!process.waitForStarted(10000))
        fail("OCRを開始できません。" + channels.error());
    struct OwnedProcess
    {
        QProcess& process;
        ~OwnedProcess()
        {
            if (process.state() != QProcess::NotRunning)
            {
                process.kill();
                process.waitForFinished(5000);
            }
        }
    } cleanup{process};
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray buffer;
    auto consume = [&]
    {
        buffer += channels.progress();
        if (buffer.size() > 1024 * 1024)
            fail("OCR進捗の形式またはサイズが不正です。");
        while (buffer.contains('\n'))
        {
            const auto end = buffer.indexOf('\n');
            const auto json = QJsonDocument::fromJson(buffer.left(end)).object();
            buffer.remove(0, end + 1);
            if (progress && json["done"].isDouble())
                progress(QString("%1 / %2ページ：%3")
                             .arg(json["done"].toInt())
                             .arg(json["total"].toInt())
                             .arg(json["status"].toString()));
        }
    };
    while (process.state() != QProcess::NotRunning)
    {
        stop(cancelled);
        if (elapsed.elapsed() > 15 * 60 * 1000)
            fail("この入力のOCRが15分の時間制限を超えました。");
        process.waitForFinished(100);
        consume();
    }
    consume();
    stop(cancelled);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        fail("OCRに失敗しました。" + channels.error());
    return job->readResult(document);
}
void publish(const PDFDocument& document, const BatchInput& input,
             const std::function<bool()>& cancelled, const std::function<void(QString)>& progress,
             QByteArray& hash)
{
    stop(cancelled);
    unchanged(input);
    if (QFileInfo::exists(input.destination))
        fail("出力先が既に存在します。既存ファイルを保持しています。");
    SaveCandidate candidate(QFileInfo(input.destination).absolutePath());
    writeCandidate(document, candidate.filePath());
    if (progress)
        progress("出力を確定する前の最終確認をしています…");
    stop(cancelled);
    unchanged(input);
    hash = fileHash(candidate.filePath());
    if (hash.isEmpty())
        fail("保存候補のハッシュを確認できません。");
    const auto from = extendedWindowsPath(candidate.filePath()),
               to = extendedWindowsPath(input.destination);
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString("出力の確定に失敗しました（Windows %1）。既存ファイルは保持しています。")
                 .arg(GetLastError()));
}
} // namespace
QString batchStatusText(BatchStatus status)
{
    switch (status)
    {
    case BatchStatus::Pending:
        return "待機";
    case BatchStatus::Running:
        return "処理中";
    case BatchStatus::Saved:
        return "保存済み";
    case BatchStatus::Unneeded:
        return "処理不要";
    case BatchStatus::Failed:
        return "失敗";
    case BatchStatus::Cancelled:
        return "取消";
    case BatchStatus::NotProcessed:
        return "未処理（中止）";
    }
    fail("処理状態が不正です。");
}
BatchPlan prepareBatch(const QStringList& inputs, const QString& outputDirectory,
                       BatchOperation operation, const QString& language,
                       const std::function<bool()>& cancelled)
{
    if (inputs.isEmpty() || inputs.size() > 100)
        fail("1〜100件のPDFを指定してください。");
    if (operation != BatchOperation::Ocr && operation != BatchOperation::Optimize)
        fail("処理の種類が不正です。");
    if (!ocrLanguageCodes().contains(language))
        fail("OCR言語が不正です。");
    if (outputDirectory.isEmpty() || !QFileInfo(outputDirectory).isDir())
        fail("既に存在する出力フォルダを指定してください。");
    BatchPlan plan;
    plan.operation = operation;
    plan.language = language;
    plan.outputDirectory = QFileInfo(outputDirectory).absoluteFilePath();
    const auto suffix = operation == BatchOperation::Ocr ? "_ocr.pdf" : "_optimized.pdf";
    for (const auto& name : inputs)
    {
        stop(cancelled);
        const QFileInfo file(name);
        if (!file.isFile() || file.suffix().compare("pdf", Qt::CaseInsensitive) != 0 ||
            file.size() < 1 || file.size() > fileLimit)
            fail("入力は空でない256MiB以内のPDFファイルで指定してください。");
        BatchInput input{
            file.absoluteFilePath(),
            QDir(plan.outputDirectory).absoluteFilePath(file.completeBaseName() + suffix),
            fileHash(file.absoluteFilePath()), file.size()};
        stop(cancelled);
        if (input.hash.isEmpty() || QFileInfo::exists(input.destination) ||
            sameFilePath(input.source, input.destination))
            fail("入力を照合できないか、出力が既に存在します。既存ファイルは保持しています。");
        for (const auto& previous : plan.inputs)
            if (sameFilePath(previous.source, input.source) ||
                sameFilePath(previous.destination, input.destination))
                fail("入力が重複しているか、同じ出力名になります。入力の名前を確認してください。");
        plan.inputs << input;
    }
    return plan;
}
QVector<BatchItemResult> processBatch(const BatchPlan& plan, const std::function<bool()>& cancelled,
                                      const BatchProgress& progress)
{
    // Revalidate the plan's shape without replacing its captured input hashes.
    if (plan.inputs.isEmpty() || plan.inputs.size() > 100 ||
        (plan.operation != BatchOperation::Ocr && plan.operation != BatchOperation::Optimize) ||
        !ocrLanguageCodes().contains(plan.language) || !QFileInfo(plan.outputDirectory).isDir())
        fail("一括処理の設定が不正です。入力を確認し直してください。");
    const auto suffix = plan.operation == BatchOperation::Ocr ? "_ocr.pdf" : "_optimized.pdf";
    for (int i = 0; i < plan.inputs.size(); ++i)
    {
        const auto& input = plan.inputs[i];
        const auto expected =
            QDir(plan.outputDirectory)
                .absoluteFilePath(QFileInfo(input.source).completeBaseName() + suffix);
        if (input.hash.size() != 32 || input.bytes < 1 || input.bytes > fileLimit ||
            QFileInfo(input.source).suffix().compare("pdf", Qt::CaseInsensitive) != 0 ||
            !sameFilePath(input.destination, expected) ||
            sameFilePath(input.source, input.destination))
            fail("一括処理の入力または出力先が不正です。");
        for (int j = 0; j < i; ++j)
            if (sameFilePath(input.source, plan.inputs[j].source) ||
                sameFilePath(input.destination, plan.inputs[j].destination))
                fail("一括処理の入力または出力先が重複しています。");
    }
    QVector<BatchItemResult> results;
    for (int index = 0; index < plan.inputs.size(); ++index)
    {
        const auto& input = plan.inputs[index];
        BatchItemResult row;
        row.source = input.source;
        row.destination = input.destination;
        auto update = [&]
        {
            if (progress)
                progress(index, row);
        };
        if (stopping(cancelled))
        {
            row.status = BatchStatus::NotProcessed;
            row.message = "中止により開始していません。保存済みの出力は保持しています。";
            results << row;
            update();
            continue;
        }
        row.status = BatchStatus::Running;
        row.message = "入力を確認しています…";
        update();
        try
        {
            stop(cancelled);
            unchanged(input);
            if (!sameFilePath(QFileInfo(input.destination).absolutePath(), plan.outputDirectory) ||
                !input.destination.endsWith(".pdf", Qt::CaseInsensitive) ||
                input.hash.size() != 32 || input.bytes < 1 || input.bytes > fileLimit ||
                sameFilePath(input.source, input.destination))
                fail("一括処理の入力または出力先が不正です。");
            Document document;
            document.open(input.source);
            document.editable();
            unchanged(input);
            if (document.pages() > 500)
                fail("この入力のページ数が500ページの処理上限を超えています。");
            PDFDocument candidate;
            bool changed = false;
            auto message = [&](QString value)
            {
                row.message = value;
                update();
            };
            if (plan.operation == BatchOperation::Ocr)
            {
                const auto recognized = recognize(document, plan.language, cancelled, message);
                candidate = recognized.document;
                row.pages = recognized.pages;
                changed = recognized.changed();
            }
            else
            {
                const auto optimized = optimizePdf(document.pdf(), cancelled, message);
                candidate = optimized.document;
                changed = optimized.smaller();
            }
            stop(cancelled);
            unchanged(input);
            if (changed)
            {
                row.message = "候補を検査して新しいPDFへ保存しています…";
                update();
                publish(candidate, input, cancelled, message, row.outputHash);
                row.status = BatchStatus::Saved;
                row.message = "新しいPDFを保存しました。入力ファイルは保持しています。";
            }
            else
            {
                row.status = BatchStatus::Unneeded;
                row.message = "変更が不要なため、新しいファイルは作成していません。";
            }
        }
        catch (const pdf::PDFException&)
        {
            row.status = stopping(cancelled) ? BatchStatus::Cancelled : BatchStatus::Failed;
            row.message = "この入力を処理できません。出力は確定していません。";
            row.outputHash.clear();
        }
        catch (const std::exception& error)
        {
            row.status = stopping(cancelled) ? BatchStatus::Cancelled : BatchStatus::Failed;
            row.message = QString::fromUtf8(error.what()).left(1000);
            row.outputHash.clear();
        }
        catch (...)
        {
            row.status = stopping(cancelled) ? BatchStatus::Cancelled : BatchStatus::Failed;
            row.message = "この入力の処理に失敗しました。入力は保持しています。";
            row.outputHash.clear();
        }
        results << row;
        update();
    }
    return results;
}
} // namespace tatsu
