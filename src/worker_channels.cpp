#include "worker_channels.h"
#include "document.h"
#include "private_temp.h"

namespace tatsu
{
WorkerChannels::WorkerChannels(QProcess& process, const QString& directory) : process(process)
{
    if (!runningInAppContainer())
        return;
    progressPath = directory + "/progress.log";
    errorPath = directory + "/error.log";
    const auto input = directory + "/stdin.empty";
    for (const auto& path : {input, progressPath, errorPath})
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            fail("OCR通信ファイルを作成できません。" + file.errorString());
    }
    process.setStandardInputFile(input);
    process.setStandardOutputFile(progressPath);
    process.setStandardErrorFile(errorPath);
}
bool WorkerChannels::usesFiles() const
{
    return !progressPath.isEmpty();
}
QByteArray WorkerChannels::progress()
{
    if (!usesFiles())
        return process.readAllStandardOutput();
    QFile file(progressPath);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset))
        return {};
    auto bytes = file.read(64 * 1024);
    offset += bytes.size();
    return bytes;
}
QString WorkerChannels::error()
{
    if (process.error() == QProcess::FailedToStart)
        return process.errorString();
    if (!usesFiles())
        return QString::fromUtf8(process.readAllStandardError().left(500));
    QFile file(errorPath);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(500)) : QString();
}
} // namespace tatsu
