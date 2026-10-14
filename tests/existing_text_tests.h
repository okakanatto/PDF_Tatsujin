#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testExistingFlowingParts(const QString& fixtures, const QString& output);
QJsonObject testExistingFlowingRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingFlowingUi(const QString& fixtures, const QString& output);
QJsonObject testExistingFlowingLines(const QString& fixtures, const QString& output);
QJsonObject testExistingFlowingLinesRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingFlowingLinesUi(const QString& fixtures, const QString& output);
QJsonObject testExistingStyledForms(const QString& fixtures, const QString& output);
QJsonObject testExistingStyledFormsRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingStyledFormsUi(const QString& fixtures, const QString& output);
QJsonObject testExistingTextCore(const QString& fixtures, const QString& output);
QJsonObject testExistingStyledText(const QString& fixtures, const QString& output);
QJsonObject testExistingStyledTextRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingStyledTextUi(const QString& fixtures, const QString& output);
QJsonObject testExistingTextRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingTextUi(const QString& fixtures, const QString& output);
QJsonObject testExistingTextUiRefusals(const QString& fixtures, const QString& output);
QJsonObject testExistingTextFonts(const QString& fixtures, const QString& output);
QJsonObject testExistingTextFontUi(const QString& fixtures, const QString& output);
QJsonObject testExistingTextMultiline(const QString& fixtures, const QString& output);
QJsonObject testExistingTextMultilineUi(const QString& fixtures, const QString& output);
QJsonObject testExistingTextRelativeLines(const QString& fixtures, const QString& output);
QJsonObject testExistingTextLineCount(const QString& fixtures, const QString& output);
QJsonObject testExistingTextLineCountUi(const QString& fixtures, const QString& output);
QJsonObject testExistingFormText(const QString& fixtures, const QString& output);
QJsonObject testExistingFormTextUi(const QString& fixtures, const QString& output);
QJsonObject testExistingTextWrap(const QString& fixtures, const QString& output);
QJsonObject testExistingTextWrapUi(const QString& fixtures, const QString& output);
} // namespace tatsu
