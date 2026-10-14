#pragma once
#include <QString>

namespace tatsu
{
// For Unicode Win32 file APIs only; keep ordinary Qt paths in document state.
QString extendedWindowsPath(const QString& path);
} // namespace tatsu
