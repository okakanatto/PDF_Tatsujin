#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
QStringList extractWordText(PDFDocument document, const QVector<int>& pages,
                            const std::function<bool()>& cancelled = {});
void exportWordText(const QStringList& pages, const QString& path, const QString& fontFamily,
                    const std::function<bool()>& cancelled = {});
} // namespace tatsu
