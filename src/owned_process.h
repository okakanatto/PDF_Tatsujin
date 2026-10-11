#pragma once
#include <QByteArray>
#include <QStringList>
#include <functional>

namespace tatsu
{
struct OwnedProcessResult
{
    int exitCode = -1;
    QByteArray output;
    bool outputTruncated = false;
};
// Worker-thread API. The Windows job owns only this invocation and its descendants.
OwnedProcessResult runOwnedProcess(const QString& program, const QStringList& arguments,
                                   const QString& directory, int timeoutMs,
                                   const std::function<bool()>& cancelled = {},
                                   int maximumOutputBytes = 65536);
} // namespace tatsu
