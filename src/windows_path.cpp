#include "windows_path.h"
#include <QDir>
#include <QFileInfo>

namespace tatsu
{
QString extendedWindowsPath(const QString& path)
{
    const auto native =
        QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (native.startsWith("\\\\?\\"))
        return native;
    if (native.startsWith("\\\\"))
        return "\\\\?\\UNC\\" + native.mid(2);
    return "\\\\?\\" + native;
}
} // namespace tatsu
