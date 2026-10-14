# 字体状態の固定試験

2026-10-12。元の固定入力・正解・閾値を変更しない。

| 入力群 | 条件 | criteria SHA-256 |
|---|---|---|
| font-binding-v1 | 同じ直格納宣言の成功1、異なる局所宣言の拒否3 | b167c53f03b5939aae9206d8bf0c38b205790413698e87c93462732b4c07f62b |
| font-binding-followup-v1 | 途中除去・後続Tj・回転90・小数座標／サイズの成功4 | 7f585559474ea31e06f94c1b1f18c40a575e0b11e7e9dd5741d473d3bc3c9720 |

M5R25_font_state_bindingとM5R26_font_state_followupは本番exportRedactedPdfで実行する。拒否で出力ファイルを作らず、原本のencode結果を保持する。成功では選択したSECRET_ALIASを削除し、別の範囲にある日本語の秘匿機密亀鶴と保持語は残す。範囲外の語を全ページから消して試験を通す方式にしない。

独立検査evaluate-redaction-font-binding.pyはPDFium／pypdfの保持語と削除語、元の各保持文字の位置0.5pt、PDFium／Poppler300dpiの範囲外差分0（旧来の境界1画素）を要求する。この条件の除去字体は標準Helveticaで、無関係な日本語の埋込み字体や対応表は保持する。除去に無関係な字体を全て消す要件は追加しない。

修正前の危険候補を調べる--probe-layout --inspect-refused-candidatesは、拒否予定なのに存在する候補をFAILにする診断用。拒否予定の出力不在を独立アプリ成功件数へ加算しない。dev29の診断2件の大きな差、正常宣言の29画素差、整数移動量の86画素差、文字原点の36／21画素差などのFAILは保持した。成功対照の入力・正解・差分0を変更せず修正した。

dev30最初のビルド出力からの起動はQt依存がなく未実行。専用引数と実行パスで確認した自分のPIDだけを終了し、依存を揃えた作業用アプリで再実行した。未実行をアプリの合格または機能FAILにしない。

ネイティブGUI・外部ビューア・実機出荷評価は未実行。新exeでの全体回帰・配布候補の最新状態はM5_FONT_BINDING_REPORT.mdとM5_FONT_BINDING_RESULTS.jsonへ記録する。
