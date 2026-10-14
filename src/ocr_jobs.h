#pragma once
#include <QLockFile>
#include <QStringList>
#include <memory>

namespace tatsu
{
QStringList cleanAbandonedOcrJobs(const QString& root);
std::unique_ptr<QLockFile> lockOwnedOcrWorker(const QString& input);
} // namespace tatsu
