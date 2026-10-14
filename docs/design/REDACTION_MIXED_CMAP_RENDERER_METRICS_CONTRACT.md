# 混在CMapの描画境界と空白選択の追加条件

2026-10-13 dev42。主入力・正解・命令・閾値はv2 criteria SHA `796b7ae0ef979583d0180c91c1020dd30405530d044daf39e83339f0b2ae326a` のまま維持する。44 PDFとv1の失敗も変更しない。元CVT32767／密256のMIAPおよび4096命令列の作用は、この機能の結果で実証済みとしない。

実r4独立検査は、19成功候補の全てに少なくとも一つの描画FAIL、25拒否にPASS、UIの署名領域・所有情報にPASSを記録した。`evidence/dev42-independent-mixed-cmap-target-r4.json` と別のsemantics報告、22失敗PDFのSHAを保存した。負のTcでは、削除語に隣接する無輪郭U+0020の起点が矩形内にあり、その移動区間の末端は外にあった。出力は文字位置をTJで補償したが、この保持すべき空白を一つ落とした。描画位置一致をUnicode使用列の一致へ代用しない。

空白の追加正条件は `fixtures/redaction-copy/mixed-cmap-v2/space-selection.json`、SHA `2168ad0c1ffd1bfb0d618d2068c80265197fb3f8f21e8f2511cf4754c26e7698`。元のmixed-value-length.pdfの第一空白だけの移動区間を完全に覆う矩形を、製品変更前に固定した。13個の保持コード・Unicode・原点を別々に照合し、第二空白と全非空白を保持する。これは空白選択・保持位置の条件で、外部描画PASSを主張する条件ではない。無輪郭U+0020の処理変更は、埋め込み横書きType0のcustom Encoding CMapへ限定する。

Popplerのr4差は、one-byte入力でも72dpiで505画素、96dpiで750画素、300dpiで0だった。CMapだけを元の字体形式へ変えた対照では差が残った。出力の命令・PDF Encoding／ToUnicode・幅を維持し、元SFNTと独立の同一輪郭署名によるCID→元GIDへ置き換えた診断対照では差が0になった。これは全元字体を含む診断専用物であり、削除データやGID閉包の合格物ではない。

さらにraw SFNTのhead全体境界8バイトだけを元の値 `[-991,-1050,2930,1810]` に戻した対照は、72／96／300dpiで全て0画素だった。lowestRecPPEMだけ、またはhhea全体指標だけの変更では差が残った。診断記録は `evidence/dev42-independent-font-header-probes.json`。全輪郭・flags・hmtx・命令・CMap・PDF文字意味はこの対照で不変である。

描画境界の限定方針を `fixtures/redaction-copy/mixed-cmap-v2/renderer-metrics.json` に02:16:44.425116 UTC、製品font変更前に固定した。SHA `7d0fdd3d8a1888796566467e385a0d8c11695c9b8b236065ba75241161b75056`、19正条件・25実使用資源。元headの36～43バイトを、signed int16・順序・保持字形の包囲を確認して複写する。適用対象は埋め込みType0 custom Encoding CMapで、1バイト／2バイト／混在と共有ページ・Form・AP N/R/Dへ資源ごとに伝える。Identity-Hと単純TrueTypeは従来の保持字形から計算する方針を維持する。不要／削除字形を追加せず、他のheader・GID閉包・maxp・CVT／命令・資源metadata・名前整理の検査を維持する。チェックサムは再計算する。

v2の位置比較は、固定元コード／CID／GIDと原点から求めた真のBezier境界を、独立に解析した出力コード／CID／GID・文字命令と出力輪郭による真のBezier境界へ比較する。PDFium charboxは別定義の観測値として残す。単純Helveticaの不変後続行には元のcharboxを使う。r4記録を変更せず、測定方法の訂正だけを `evidence/dev42-independent-r4-geometry-method-correction.json` に残した。位置0.5pt、72／96／300dpi・境界1px外の0画素、対象外ページ全画素一致の閾値は維持する。

診断対照の結果は製品PASSではない。実ビルド・Qt実行の後に、実出力のhead raw境界・文字意味・実使用列・全資源の局所閉包と同一元SHAの最終union・削除データ・位置・両エンジン描画を再検査する。元PDFiumのコード長衝突が対象外へ及ぶ直接比較FAILは、Poppler修正や別scopeの意味検査PASSで埋めない。一般CMap、M1／M3／M5の完成条件は保留する。

r5r1の実出力は、全Poppler比較で0画素となり、Unicode／GID／閉包／削除データ／UI状態と追加空白条件の別scope46件はPASSになった。ただし全条件では40 PASS／6 FAILで、既知の元PDFium衝突5件に加え、TJ-positive-negativeのPDFium300dpiに8画素差が残った。`evidence/dev42-independent-mixed-cmap-target-r5r1.json` とsemantics報告、失敗8 PDFを保持する。

このTJ差の数値対照を、変更前の02:33:45.939039 UTCに `evidence/dev42-independent-TJ-numeric-probe-freeze.json`、SHA `4a4a934244310e14e661503dabd7c447c87e8a309b68b64df97a8f38cdc10571` で固定した。元TJの調整は125／-75、Tf22・Tc1.25・Tz85、削除6字の元幅はいずれも1000である。製品の削除移動量のfloat集約による `-6415.9088134766` を、同じ式のdouble値 `-6415.909090909091` へだけ変えた対照は、PDFium／Poppler72／96／300dpiで0画素となった。個々の削除字形の幅や字数を新しい命令列へ列挙する必要はなく、一つの集約値で一致する。入力・文字意味・字体・矩形・閾値は不変。最初の対照生成では数値トークンの接頭部分だけを置換して余りの桁を残す誤りがあり、失敗PDFと位置FAILを保存した。完全な数値トークンを置換した同じ固定対照の結果を `evidence/dev42-independent-TJ-numeric-probe-results.json` に残した。診断の0画素を、修正後の製品実行PASSへ代用しない。

最終r6実出力の独立検査は、`evidence/dev42-independent-mixed-cmap-target-r6.json` で全条件41 PASS／5 FAIL、別のsemantics報告で46 PASS／0 FAILとなった。元PDFium衝突のcode32-nonspace・共有ページ・共有Form・共有AP・UI直接描画の5 FAILは保持する。19正入力／25実使用資源のhead境界8バイト・局所GID閉包・同一元SFNTの最終参照union・保持Unicode使用列・削除データ・位置が一致した。負のTcは二つのU+0020を保持し、追加空白条件は13字を保持する。全Poppler72／96／300dpiと、TJのPDFium72／96／300dpiは0画素。失敗7 PDFのパス／SHAを主報告に固定した。実exe SHA `7a7fdb1bad2f083c9c8f52c7d9f7d67b63d8ddacb92d2600f1399fa00aee726b`、producer／report／evaluatorの同一性を `evidence/dev42-independent-r6-bindings-final.json` に照合した。一般機能の完成や元エンジンの描画制約解消は宣言しない。
