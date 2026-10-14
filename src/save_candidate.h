#pragma once
#include "private_temp.h"
#include <QLockFile>

namespace tatsu
{
// Only marked, flat save candidates with no active writer are eligible.
QStringList cleanAbandonedSaveCandidates(const QString& parent);

class SaveCandidate
{
public:
    explicit SaveCandidate(const QString& parent);
    QString filePath() const;

private:
    // Reverse member destruction releases the lease before removing the directory.
    std::unique_ptr<PrivateTemporaryDirectory> directory;
    std::unique_ptr<QLockFile> lease;
};
} // namespace tatsu
