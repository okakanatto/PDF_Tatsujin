#pragma once
#include "document.h"

namespace tatsu
{
struct PdfaInput
{
    PDFDocument document;
    QString source;
    QByteArray sourceHash;
};
struct PdfaRule
{
    QString specification, clause, test, description;
    int failedChecks = 0;
};
struct PdfaValidation
{
    bool compliant = false;
    QString profile, engineVersion, snapshotHash;
    int failedRules = 0, failedChecks = 0;
    QVector<PdfaRule> rules;
};
PdfaInput currentPdfaInput(const Document& document);
PdfaValidation parsePdfaReport(const QByteArray& xml, const QString& profile, int exitCode);
PdfaValidation validatePdfa(const PdfaInput& input, const QString& java, const QString& jar,
                            const QString& profile, const std::function<bool()>& cancelled = {});
QJsonObject pdfaResultJson(const PdfaValidation& result);
void exportPdfaResult(const PdfaValidation& result, const QString& path);
} // namespace tatsu
