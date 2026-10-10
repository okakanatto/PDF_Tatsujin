#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct ExistingTextBlock
{
    int occurrence = 0;
    QString text, font, restriction;
    QRectF physical;
};
enum class ExistingTextChange
{
    Geometry,
    Replace,
    Remove
};
QVector<ExistingTextBlock> existingTextBlocks(const PDFDocument& document, int page,
                                              const std::function<bool()>& cancelled = {});
PDFDocument editExistingText(const PDFDocument& snapshot, int page, int occurrence,
                             ExistingTextChange change, const QString& text = {},
                             QRectF physical = {}, const std::function<bool()>& cancelled = {});
PDFDocument replaceExistingTextFont(const PDFDocument& snapshot, int page, int occurrence,
                                    const QString& text, const QString& family,
                                    const std::function<bool()>& cancelled = {});
} // namespace tatsu
