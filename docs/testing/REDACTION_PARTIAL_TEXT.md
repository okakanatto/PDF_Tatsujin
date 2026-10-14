# 文字列途中と既存OCRの墨消し検証

製品ソース `dcc0755891a379c34719a9085e820505d1db5134`、Windows x64／Qt6.9.3／MSVC19.50、0.5.0-dev24。通常215＋別NTFS1／Microsoft Print to PDF1＝217件がPASS。実際の全体回帰は2回。初回の217件PASS後に追加の複合字形で1条件FAILを再現したため修正・再試験した。以前の212試験名・入力・正解・閾値・FAILを保持する。

対応する横書きTj／TJ、標準字体、命令のない保持字形を持つTrueType、標準Device色空間・不透明な描画状態、軸に沿うType1 Formを対象にする。指定字形と画像画素を除去し、範囲外の字形を再構築して検索・コピー・位置を保持する。元の字体対応表と字体本体・不要な依存を除去し、共有参照や複製が残る候補は拒否する。字形を横切る指定、未評価の内容構造は成功扱いにしない。

12固定合成入力＋複合部品2入力＋実D03の既存OCR1条件、合計15の独立検査がPASS。PDFium／pypdfで対象語不在と保持語、fontToolsで字体対応と分解後の輪郭、PDFium／Poppler300dpiで指定矩形＋既存1画素境界の外側の差分0を検査した。位置差最大0.285949707ptで固定0.5pt以内。実D03の保持語は位置差0で、対象外英語ページの描画も一致する。M1の別エンジン検査32条件と、77 PE／9資産／106通知の静的監査もPASS。

固定追加条件を読む場所は `fixtures/redaction-copy/partial-text/criteria.json`、`partial-composite-v3/criteria.json`、`partial-real-ocr.json`。既存のスキャンOCR前矩形や検索語は変更しない。旧非対応入力の字体を再構築できるようになったため、元の入力のまま出力の安全性と保持を検査する。元の拒否・失敗記録も保持する。

新しい実行依存は追加していない。通常の `scripts/build.ps1 -Target PDFTatsujin` でビルドし、`scripts/run-tests.ps1 -Headless` で同梱部品を使う。独立検査は `evaluate-redaction-partial.py` と `evaluate-redaction-composite.py`、原本・候補・実行記録は新規出力先で保存する。Windows／外部エンジン／Poppler等の実経路は `docs/DEVELOPMENT.md` に従う。

M5全体の一般文書安全性、ネイティブGUI／IME／OS倍率・クリップボード、Reader／WordGUI、クリーンWindows、物理印刷、実容量不足、第三者／Acrobat比較、配布主体のMSVC／SDK条件は未実行／未完了。Poppler文字抽出はpdftotext未配置のため未実行。詳細設計は [REDACTION_PARTIAL_TEXT.md](../design/REDACTION_PARTIAL_TEXT.md)。
