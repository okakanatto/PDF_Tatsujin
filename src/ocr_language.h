#pragma once
#include <QStringList>

namespace tatsu
{
inline QStringList ocrLanguageCodes()
{
    return {"jpn+eng", "jpn", "eng", "jpn_vert"};
}
inline QStringList ocrLanguageLabels()
{
    return {"日本語＋英語", "日本語", "英語", "日本語（縦書き）"};
}
} // namespace tatsu
