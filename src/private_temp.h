#pragma once
#include <QTemporaryDir>
#include <memory>

namespace tatsu
{
bool runningInAppContainer();
class PrivateTemporaryDirectory
{
public:
    explicit PrivateTemporaryDirectory(const QString& pattern);
    ~PrivateTemporaryDirectory();
    PrivateTemporaryDirectory(const PrivateTemporaryDirectory&) = delete;
    PrivateTemporaryDirectory& operator=(const PrivateTemporaryDirectory&) = delete;
    bool isValid() const;
    QString path() const;
    QString filePath(const QString& name) const;

private:
    std::unique_ptr<QTemporaryDir> normal;
    QString location;
};
std::unique_ptr<PrivateTemporaryDirectory> privateTemporaryDirectory(const QString& pattern);
} // namespace tatsu
