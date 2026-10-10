#pragma once
#include <QJsonObject>
#include <QStringList>
namespace tatsu
{
class Window;
void verifyOfficeSearchCopy(Window& window, int page, const QStringList& phrases);
QJsonObject testOfficeImport(const QString& fixtures, const QString& output);
QJsonObject testOfficeImportUi(const QString& fixtures, const QString& output);
QJsonObject testOwnedProcess(const QString& output);
QJsonObject testOfficeImportWindow(const QString& fixtures, const QString& output);
QJsonObject testOfficeSheetsSlides(const QString& fixtures, const QString& output);
QJsonObject testOfficeSheetsSlidesUi(const QString& fixtures, const QString& output);
} // namespace tatsu
