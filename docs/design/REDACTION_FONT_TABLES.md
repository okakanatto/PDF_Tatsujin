# 再構築字体のヘッダ・集計値

2026-10-12。dev25の残る字体内情報を固定入力で確認し、hheaの予約領域、headの版情報、maxpの未使用ヒント集計値へ入れたデータが出力へ残る旧候補を再現した。旧34条件のPASSと別のFAILとして扱い、旧exe・出力を保持する。一般M5合格とは扱わない。

再構築後のhead／hhea／maxpを新規生成する。予約領域、メーカー版、日時、未使用ヒント用の領域サイズを引き継がない。保持する字形の輪郭・複合部品閉包・進み量・side bearingから、全体bbox、最大幅・bearing・extent、点／輪郭数と複合深さを再集計する。スケール、PDFの範囲外描画に必要な既知のheadフラグ、hheaのascent／descent／lineGapは既存配置を保持するため明示的に引き継ぐ。未知の表やヒント命令を許可する例外は作らない。

単純字形の宣言bboxは実際のon／off curve制御点と照合し、不一致は失敗時非反映で拒否する。輪郭のない字形の不要なbbox・データは空へ正規化する。複合字形は部品閉包の点・輪郭数と深さを再集計し、必要なGID0も維持する。一般複合変換・hint解釈の対応条件は広げない。

固定追加入力font-tables-v1は予約領域、版情報、ヒント集計、3種類併用の成功4条件と不正な単純字形bboxの拒否1条件。既存visible.pdfの秘密語・保持語・指定矩形を維持する。元fontと私的markerの不在、字体輪郭・0.5pt、PDFium／Poppler300dpi範囲外画素差0を要求し、既存34条件と新4条件を独立に検査し、合計は実検証後に報告する。未実行をPASSにしない。

仕様は [OpenType head](https://learn.microsoft.com/en-us/typography/opentype/spec/head)、[hhea](https://learn.microsoft.com/en-us/typography/opentype/spec/hhea)、[maxp](https://learn.microsoft.com/en-us/typography/opentype/spec/maxp)、[glyf](https://learn.microsoft.com/en-us/typography/opentype/spec/glyf)。hheaの予約領域はゼロ、全体bboxとbearingは輪郭がある字形から計算する。maxpは保持字体の資源量を表す。glyfのbboxはon／off curve制御点に基づく。

小変更では梱包せず、今回の実際に再現した安全性不具合の修正と検証をまとめる。
