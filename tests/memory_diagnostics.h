#pragma once
#include <QJsonObject>

namespace tatsu::diagnostics
{
QJsonObject defaultHeap();
QJsonObject memorySnapshot();
QJsonObject trimHeapCaches();
} // namespace tatsu::diagnostics
