#pragma once
#include <QProcess>

namespace tatsu
{
// Qt's ordinary named pipes cannot be created inside an AppContainer.
// In that context, poll private local files without adding IPC capabilities.
class WorkerChannels
{
public:
    WorkerChannels(QProcess& process, const QString& directory);
    QByteArray progress();
    QString error();
    bool usesFiles() const;

private:
    QProcess& process;
    QString progressPath, errorPath;
    qint64 offset = 0;
};
} // namespace tatsu
