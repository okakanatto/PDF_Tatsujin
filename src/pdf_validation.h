#pragma once
#include "document.h"

namespace tatsu
{
struct PdfValidationProfile
{
    QString id, title, reportName;
    bool machineOnly = false;
};
const QVector<PdfValidationProfile>& pdfValidationProfiles();
const PdfValidationProfile& pdfValidationProfile(const QString& id);
struct PdfValidationInput
{
    PDFDocument document;
    QString source;
    QByteArray sourceHash;
};
struct PdfValidationRule
{
    QString specification, clause, test, description;
    int failedChecks = 0;
};
struct PdfValidationResult
{
    bool checksPassed = false;
    QString profile, engineVersion, snapshotHash;
    int failedRules = 0, failedChecks = 0;
    QVector<PdfValidationRule> rules;
};
PdfValidationInput currentPdfValidationInput(const Document& document);
PdfValidationResult parsePdfValidationReport(const QByteArray& xml, const QString& profile,
                                             int exitCode);
PdfValidationResult validatePdfConformance(const PdfValidationInput& input, const QString& java,
                                           const QString& jar, const QString& profile,
                                           const std::function<bool()>& cancelled = {});
QJsonObject pdfValidationResultJson(const PdfValidationResult& result);
void exportPdfValidationResult(const PdfValidationResult& result, const QString& path);
} // namespace tatsu
