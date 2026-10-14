#pragma once
#include <QChar>

namespace tatsu
{
inline bool keepVerticalJapaneseSpacing(double angle, QChar previous, QChar current)
{
    if (angle != 90 && angle != 270)
        return false;
    const auto japanese = [](QChar character)
    {
        const auto script = character.script();
        return script == QChar::Script_Han || script == QChar::Script_Hiragana ||
               script == QChar::Script_Katakana ||
               (character.unicode() >= 0x3001 && character.unicode() <= 0x303f);
    };
    // Keep encoded spaces. A larger vertical pitch between Japanese glyphs is
    // tracking, not evidence of a new word separator.
    return japanese(previous) && japanese(current);
}
} // namespace tatsu
