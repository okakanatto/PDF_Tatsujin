# 同参照の字体別名：固定試験

2026-10-12。旧入力・正解・閾値を保持して、別の字体宣言を同名で使う既存試験とは別に、同じ字体オブジェクトを複数の資源名で使う条件を固定した。

| 入力群 | 件数 | criteria.jsonのSHA-256 | 固定時点 |
|---|---:|---|---|
| font-aliases-v1 | 成功9・拒否2 | 06fd7bbcef23bf82d58852c351bd3d0d5098b96cbfea1ea23d5639e61332bcd9 | 実装前 |
| font-aliases-followup-v1 | 成功3 | 4aa425628996b0a58f49d8f99b88421e19db3a3d61f355553607118519687550 | 実装後、追試前 |
| font-aliases-boundaries-v1 | 拒否2 | e0bef06a28cfd3ae32ccb1c09c67baa8373259fbfdb63eede56ef4b1b3aadf32 | 辞書欠落ガード前、追試前 |

M5R23_font_resource_aliasesは16条件で本番のexportRedactedPdfを使い、成功語／対象語、原本不変、拒否時出力不在を要求する。M5R24_font_resource_aliases_UIは未保存署名の保持、保存されたコピーの再編集、原本Undo／Redo、拒否時の変更非反映、取消の先頭・中間・末尾を確認する。Qt offscreenの確認であり、ネイティブ画面・IME・OSクリップボードは未実行。

evaluate-redaction-shared.pyは入力群の固定SHAを照合する。同一の独立検査で保持語、元字体・ToUnicode・選択内容の不在、fontTools輪郭、位置0.5pt、PDFium／Poppler300dpi範囲外差分0と対象外ページ全画素差0を要求する。旧shared-fonts-v1のpinと条件は変更しない。--probe-layoutは診断候補の検査用で、成功をアプリの試験名へ加算しない。

修正前dev28プローブは成功予定9件中2PASS・4文字保持FAIL・3処理拒否、拒否予定2件は拒否した。欠落出力の独立検査は未実行として記録し、実行済みの処理拒否3件は機能未達と別に明記する。初期dev29の継承字体追試FAILも保持する。最新版の集約・実行バイナリ・回帰結果はM5_FONT_ALIASES_RESULTS.jsonを参照する。

再現は、新しい出力先へscripts/build.ps1 -Target PDFTatsujinでビルドし、TATSU_TEST_FILTER=M5R23またはM5R24でscripts/run-tests.ps1 -Headlessを実行する。AppDirectoryは作業ルートからの相対パスを渡す。独立検査はscripts/evaluate-redaction-shared.pyの--fixtures、--run、--outputで指定する。全体回帰はOffice／PDF-Aの固定エンジンを設定してから実行する。
