#pragma once

#include <QStringList>

namespace tatsu
{
// Runs in an isolated process. Only a fully completed job publishes an output PDF.
int ocrWorker(const QStringList& args);
} // namespace tatsu
