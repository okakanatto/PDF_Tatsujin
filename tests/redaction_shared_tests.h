#pragma once
#include <QtCore>
namespace tatsu
{
QJsonObject testRedactionFontTables(const QString& fixtures, const QString& output);
QJsonObject testRedactionHinting(const QString& fixtures, const QString& output);
QJsonObject testRedactionHintingUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionHintBoundaries(const QString& fixtures, const QString& output);
QJsonObject testRedactionHintBoundariesUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionCvtReferences(const QString& fixtures, const QString& output);
QJsonObject testRedactionCvtReferencesUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionCvtReachability();
QJsonObject testRedactionCvtIndexBoundaries(const QString& fixtures, const QString& output);
QJsonObject testRedactionCvtIndexBoundariesUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionCustomCMap(const QString& fixtures, const QString& output);
QJsonObject testRedactionCustomCMapUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionMixedCMap(const QString& fixtures, const QString& output);
QJsonObject testRedactionMixedCMapUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionMixedWordSpacing(const QString& fixtures, const QString& output);
QJsonObject testRedactionMixedWordSpacingUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionCMapFollowup(const QString& fixtures, const QString& output);
QJsonObject testRedactionRedundantClips(const QString& fixtures, const QString& output);
QJsonObject testRedactionRedundantClipsUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionFontAliases(const QString& fixtures, const QString& output);
QJsonObject testRedactionFontAliasesUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionFontBinding(const QString& fixtures, const QString& output);
QJsonObject testRedactionFontBindingFollowup(const QString& fixtures, const QString& output);
QJsonObject testRedactionFontSchema(const QString& fixtures, const QString& output);
QJsonObject testRedactionFontSchemaUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionDirectFontSchema(const QString& fixtures, const QString& output);
QJsonObject testRedactionDirectFontSchemaUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionStandardFontFamilies(const QString& fixtures, const QString& output);
QJsonObject testRedactionShared(const QString& fixtures, const QString& output);
QJsonObject testRedactionSharedFailures(const QString& fixtures, const QString& output);
QJsonObject testRedactionSharedUi(const QString& fixtures, const QString& output);
QJsonObject testRedactionPaths(const QString& fixtures, const QString& output);
QJsonObject testRedactionSharedFollowup(const QString& fixtures, const QString& output);
} // namespace tatsu
