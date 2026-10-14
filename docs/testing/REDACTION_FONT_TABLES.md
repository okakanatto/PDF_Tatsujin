# 字体ヘッダ・集計値の再構築検証

0.5.0-dev26、Windows11 x64／Qt6.9.3／MSVC19.50。dev25の222名／独立34条件のPASS後、字体内のhhea予約領域、head版情報、maxp未使用hint集計へ入れた私的markerが残る旧出力を追加固定入力で再現した。旧結果は保護し、dev25をこの追加条件の成功にしない。

head／hhea／maxpを新規生成し、保持する字形から全体bbox・bearing／extent・点／輪郭数・複合深さを再集計する。メーカー版・日時・予約領域・未使用hint資源は引き継がない。配置に必要な既知のスケール／描画フラグ・ascent／descent／lineGap等を明示的に保持する。単純字形の宣言bboxと制御点は照合して不一致を拒否し、空の輪郭は不要なbboxとデータを残さない。

追加固定font-tables-v1のcriteria SHA-256は7a8d6e747eba9816486ec202aac858acc1bdbf686c9c0931808fc9f725981cdd。成功4／拒否1をM5R17で処理する。アプリの墨消し17名と、旧34＋新4＝38の独立条件はPASS。輪郭一致、位置0.5pt以内、PDFium／Poppler300dpiの範囲外差分0とprivate marker不在、fontToolsによるhead／hhea／maxpの独立再集計を要求する。最大位置差0.285949707pt。旧固定入力・正解・検索語・閾値は変更していない。

追加empty-glyph-v1のcriteria SHA-256は229109449cedde155678515bd2a6bb628d0602203541289e0e40b271f1ccf5d1。空白字形に描画上不要なbbox情報を含む1入力を固定し、旧プローブでmarker残存を再現、同じ修正ライブラリのプローブで除去と元advance／輪郭／位置／範囲外画素差0を確認した。この診断1条件をアプリ全体の1名に数えない。

全体221（画面配置含む）＋Windows2＝223名、M1独立32と旧コピー12／スキャン2もPASS。旧222名を保持してM5R17を追加、今回は全体1回。梱包後の確定結果は対応JSONを参照する。未完了を成功へ数えない。設計は [REDACTION_FONT_TABLES](../design/REDACTION_FONT_TABLES.md)。入力生成はprepare-redaction-font-tables.py／prepare-redaction-empty-glyph.py、独立検証はevaluate-redaction-font-tables.py。元の危険出力、旧exe、生成・ビルド・起動失敗を保持する。

保持hint、CFF／可変字体、独自CMap／多文字Unicode、DR／DA、異なる範囲の共有Form、透明合成・部分図形・複雑clipと一般安全性は未完了。実機入力・倍率、外部実画面、クリーンWindows、物理印刷、実容量不足、第三者評価、一般配布条件、新exeの通信能力ゼロは未実行／未完了。M5全体合格・一般公開・Acrobat同等とは扱わない。
