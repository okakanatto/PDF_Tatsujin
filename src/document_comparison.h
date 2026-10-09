#pragma once
#include "document.h"
#include <functional>

namespace tatsu
{
struct ComparisonOptions
{
    bool matchCommonPages = true;
};
struct ComparedPage
{
    int left = -1, right = -1;
    bool geometryChanged = false, textChanged = false, pixelsChanged = false, formsChanged = false,
         annotationsChanged = false;
    QRect pixelBounds;
    QByteArray leftPixels, rightPixels;
    QString leftText, rightText;
    QJsonArray leftForms, rightForms, leftAnnotations, rightAnnotations;
    bool changed() const;
    QString description() const;
};
struct ComparisonResult
{
    QVector<ComparedPage> pages;
    QJsonObject leftProperties, rightProperties;
    QStringList propertyChanges;
    int leftPages = 0, rightPages = 0;
    ComparisonOptions options;
    int changedPages() const;
    bool same() const;
};
QImage renderComparisonPage(PDFDocument& document, int page);
ComparisonResult compareDocuments(PDFDocument left, PDFDocument right,
                                  const ComparisonOptions& options = {},
                                  const std::function<bool()>& cancelled = {},
                                  const std::function<void(QString, int, int)>& progress = {});
QJsonObject comparisonJson(const ComparisonResult& result);
void exportComparison(const ComparisonResult& result, const QString& destination,
                      const std::function<bool()>& cancelled = {});
} // namespace tatsu
