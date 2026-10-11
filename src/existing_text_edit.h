#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
PDFDocument replaceExistingTextWrapped(const PDFDocument& snapshot, int page, int occurrence,
                                       const QString& text, double width, double leadingRatio,
                                       const QString& family = {},
                                       const std::function<bool()>& cancelled = {},
                                       bool includeForms = false);
struct ExistingTextBlock
{
    int occurrence = 0;
    QString text, font, restriction;
    QRectF physical;
    int depth = 0;
};
enum class ExistingTextChange
{
    Geometry,
    Replace,
    Remove,
    Lines
};
QVector<ExistingTextBlock> existingTextBlocks(const PDFDocument& document, int page,
                                              const std::function<bool()>& cancelled = {},
                                              bool includeForms = false);
PDFDocument editExistingText(const PDFDocument& snapshot, int page, int occurrence,
                             ExistingTextChange change, const QString& text = {},
                             QRectF physical = {}, const std::function<bool()>& cancelled = {},
                             bool includeForms = false);
PDFDocument replaceExistingTextFont(const PDFDocument& snapshot, int page, int occurrence,
                                    const QString& text, const QString& family,
                                    const std::function<bool()>& cancelled = {},
                                    bool includeForms = false);
PDFDocument replaceExistingTextLines(const PDFDocument& snapshot, int page, int occurrence,
                                     const QString& text, double leadingRatio,
                                     const QString& family = {},
                                     const std::function<bool()>& cancelled = {},
                                     bool includeForms = false);
} // namespace tatsu
