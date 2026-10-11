#include "body_text_wrap.h"
#include "document.h"
#include <cmath>

namespace tatsu
{
QString wrapBodyText(const QString& text, double width,
                     const std::function<QVector<BodyGlyphExtent>(const QString&)>& measure,
                     const std::function<bool()>& cancelled)
{
    auto stop = [&]
    {
        if (cancelled && cancelled())
            fail("本文の折返しを中止しました。");
    };
    stop();
    if (text.isEmpty() || text.size() > 4096 || !std::isfinite(width) || width < 1)
        fail("4096文字以内の本文と1pt以上の折返し幅を指定してください。");
    for (const auto& c : text)
        if (c.isNull() || (c.isSpace() && c != ' ' && c != '\n') || c.isSurrogate())
            fail("この制御文字や補助文字を含む本文の折返しは未対応です。");
    QStringList rows;
    for (const auto& paragraph : text.split('\n'))
    {
        if (paragraph.isEmpty() || paragraph.trimmed() != paragraph)
            fail("空行や行頭・行末の空白を除いて入力してください。");
        const auto glyphs = measure(paragraph);
        if (glyphs.size() != paragraph.size())
            fail("本文の字形と文字の対応を確認できません。");
        QTextBoundaryFinder clusters(QTextBoundaryFinder::Grapheme, paragraph);
        QVector<int> boundaries;
        for (auto p = clusters.toNextBoundary(); p >= 0; p = clusters.toNextBoundary())
            boundaries << int(p);
        QTextBoundaryFinder line(QTextBoundaryFinder::Line, paragraph);
        int start = 0;
        while (start < paragraph.size())
        {
            stop();
            double advance = 0, left = 0, right = 0;
            int end = start, preferred = -1;
            bool overflow = false;
            for (int boundary : boundaries)
            {
                if (boundary <= start)
                    continue;
                for (int i = end; i < boundary; ++i)
                {
                    const auto& glyph = glyphs[i];
                    if (!std::isfinite(glyph.advance) || glyph.advance < 0 ||
                        !std::isfinite(glyph.left) || !std::isfinite(glyph.right))
                        fail("本文の字形幅を確認できません。");
                    left = qMin(left, advance + glyph.left);
                    right = qMax(right, advance + glyph.right);
                    advance += glyph.advance;
                }
                if (qMax(advance, right) - left > width + 1e-8)
                {
                    overflow = true;
                    break;
                }
                end = boundary;
                line.setPosition(end);
                if (line.isAtBoundary())
                    preferred = end;
            }
            if (end == start)
                fail("折返し幅に1字形も入りません。幅を広げてください。");
            if (overflow && preferred > start)
                end = preferred;
            const auto row = paragraph.mid(start, end - start).trimmed();
            if (row.isEmpty())
                fail("折返し幅が狭すぎます。幅を広げてください。");
            rows << row;
            if (rows.size() > 64)
                fail("折返し後の本文が64行を超えます。幅を広げてください。");
            start = end;
            while (start < paragraph.size() && paragraph[start] == ' ')
                ++start;
        }
    }
    stop();
    return rows.join('\n');
}
} // namespace tatsu
