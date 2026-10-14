# 限定TrueType CVT番号の次の境界試験案

2026-10-12。dev40の閉じたMIAP文法を変えず、元CVT番号32767と密化後番号255/256を分けて調べる次の設計案。ここではPDF生成、入力・正解のpin固定、製品変更、ビルド、試験を実行していない。既存SPEC/TECH/ACCEPTANCE、入力・正解・検索語・閾値、dev35配布候補、失敗記録を維持する。

dev40の実行済み範囲は既存cvt-references-v1の限定入力と実Qt結果に限る。次の番号境界はその保持PASSから推測で合格にしない。CVT値の符号、元番号、密番号、輪郭の点番号はそれぞれ独立した意味である。

## 入力を生成する前の固定

最初の基礎入力はpartial-text/visible.pdfの既存SHA `f9bacb0ca4b4b19f5bc100e68bce571dbb474b55665ad6dbfb8097e7c9f2fd39` とする。cvt-references-v1/criteria.jsonの既存SHAは `1234729946a784d36e2d32bca4fc1e10642b01c3e900539996f236a440858be7`、hint-boundaries-v1/criteria.jsonは `1fcd232da7b7ae1af28430c8fdb7a0e92318c72895637a258f66c874dd3e4941`。生成担当は参照前に照合し、既存ファイルを書き換えない。新しい保存先候補はfixtures/redaction-copy/cvt-index-boundaries-v1であり、まだ作成済みの入力群ではない。

生成時に残る文字から異なる非空単純GIDを2個選び、PDF資源binding・CID/GID・ToUnicode・原輪郭SHAを記録する。1個を主字形、もう1個を伴う字形とする。密番号256の成功入力では2字形が同じ最終FontFile2を必要とする同じPDF字体資源に属することを固定する。主字形の実点を257点へ増やす場合は既存境界入力と同じ方法で最初の点座標・flagを複製し、元の輪郭終点を増分移動する。生成前に元点数が257以下で、最初のflagがon-curve（flag & 1）であることをassertする。off-curve先頭の複製はimplied on-curve点と曲線を変え得るため、この合成方法では受け入れない。元の座標列・形状・bbox・advanceを保持し、生成済み入力から正解を固定する。原字体とNotoの利用条件を維持し、追加した合成試験内容のみCC0とする。

全保持字形の命令は既存のSVTCA→canonical PUSH実点→MDAP[1]またはcanonical PUSH CVT→MIAP[1]→対応IUPだけ。軸順、昇順一意の点番号、real pointのみ、4096 bytes以下、空/既存固定dropout prep、fpgm・CVT書込み・算術・storage・zone変更・複合自身ヒント拒否を維持する。下表で指定しない保持字形はMDAPのみとして新たなCVT参照を加えない。

## 最初に分離する4条件

| profile案 | 元入力の固定条件 | 出力の固定正解 |
| --- | --- | --- |
| source-index-32767 | 元cvtは32768 FWORD以上。実点0→正規PUSHW `b8 7f ff`→MIAP[1]。必要参照は元32767のみ。 | 新cvtはそのsigned FWORDの2 bytesだけ。元32767→密0、CVT operandは `b0 00`。実点PUSH、他命令、輪郭は保持。 |
| source-index-negative-32768 | 元cvtを32769 FWORD以上にして、unsigned 32768なら表内となる条件を先に作る。実点0→`b8 80 00`→MIAP[1]。 | PUSHWのsigned値は-32768なので非反映拒否。表の大きさによる範囲外拒否で符号検査を代用しない。 |
| dense-index-255 | 主字形の点p=0..255が元CVT番号512-pを参照し、点256はMDAP[1]、最後にIUP[x]。同じ最終字体の必要参照集合は257..512の256個。 | 昇順257..512→密0..255。点0の元512→密255は `b0 ff`。点256の実点PUSHW `b8 01 00` は書き換えない。新cvtは512 bytes。 |
| dense-index-256 | 主字形は直前と同じ。伴う字形の実点0が元513だけをMIAP[1]し、対応IUPで閉じる。同じ最終字体の必要参照集合は257..513の257個。 | 主字形の密0..255はPUSHB。伴う字形の元513→密256は `b8 01 00`、実点0は `b0 00`のまま。新cvtは514 bytes。 |

各FWORDはその操作が参照する元実点の座標値から生成前に決める。同値が複数元番号に現れても番号ごとに別要素とし、値の一致で統合しない。dense条件の元cvtは必要範囲より大きく作り、未参照値と除去専用値へ別の連続私的markerを配置する。元32767条件では上位表の未参照値、符号条件ではunsigned 32768位置の値も記録するが、製品へその値を許可する正解にはしない。

密化は**最終ローカルFontFile2ごとの保持閉包**で行う。元payload SHA別の全共有参照unionはCVT受入のwitnessであり、別ローカル字体の番号を共通の密番号へする規則ではない。例えば共有先が主字形だけ/伴う字形だけに分かれる派生入力では、前者は256要素、後者は1要素で元513→密0となる。global unionが257個でも後者の正解を密256にしない。

主字形の片軸命令は元1542 bytes、密化後1286 bytesを期待する。内訳は256個のMIAPで点PUSHB(2)+元CVT PUSHW(3)+MIAP(1)、点256のPUSHW/MDAP(4)、SVTCA/IUP(2)。書換後はCVTだけPUSHB(2)になる。伴う字形の元513→密256は両方PUSHWで8 bytesのまま。独立検査で各命令列から長さを再計算し、ここに記した算術を製品のmaxpから逆算しない。

## 次の共有・複合派生案

上の4条件の入力と正解を固定した後、必要な複合子/GID0子に境界MIAPを置く入力、別ページ/別名字体/兄弟Form/N-R-D外観の共有先に境界MIAPを置く入力を別条件で追加する。複合自身の命令は空に保つ。残るMDAPだけのローカル字体にcvt表がないこと、同じsource SHAの到達MIAPだけがwitnessになること、上書きされ未到達になった中間字体と別source SHAがwitnessにならないことを確認する。元payloadが違うものをCVTの値だけで同一とみなさない。

## 独立監査と既存閾値

入力・期待集合・各glyphの期待命令・cvt exact bytes・元番号→密番号・期待stack/命令長を製品実行前に保存し、別担当がSHAを照合する。出力から正解を作らない。PDF資源binding→GID→複合閉包を元PDFから復元し、全保持輪郭・flags・metrics・他命令、無参照glyfの不在、全FontFile2との対応を検査する。maxStackElementsは実最大2、maxSizeOfInstructionsは書換後の最大、head/hhea/maxpは保持輪郭から再算出し、sfntの各table checksumとcheckSumAdjustmentも独立確認する。

PDFium/pypdfの位置は既存0.5pt以内、PDFium/Popplerの72/96/300dpiは既存1px矩形境界外の差0、対象外ページは全画素一致。低DPI作用は別条件として、source-index-32767ではその番号を使う対象字形だけ、dense-index-256では伴う字形の元513/密256を使うMIAP操作だけを除いた対照PDFを生成時に固定する。他字形、CVT値、輪郭、軸/IUP、MDAPは同じにする。対象操作自身の描画差が観測できなければ未実証と記録し、別字形のヒント差で代用しない。エンジン別の差は別集計する。

削除専用値・未参照値・私的marker・元cvt全体・旧字体・旧対応表の不在を全到達/未到達/展開ストリームとPDF生バイトで確認する。共有先の必要値は正当に残るので、必要な単字/値の存在と削除文字列の不在を区別する。取消、非反映拒否、出力不在、未保存署名保持、Undo/Redo、保存後再編集を実Qtで確認し、製品実行記録なしの出力不在を拒否PASSにしない。

## 容量と実行順

この設計記録以外の生成物は今回作らない。32768/32769 FWORDの元cvtは65536/65538 bytes以上となるので、圧縮PDF容量だけでピークを見積もらない。

基礎4条件を優先し、固定→製品実行→独立検査→必要回帰の順。限定境界の成功を一般CVTや一般M5、一般バイナリ公開の判断へ拡張しない。

根拠：[Microsoft TrueType命令仕様](https://learn.microsoft.com/en-us/typography/opentype/spec/tt_instructions)、[cvt](https://learn.microsoft.com/en-us/typography/opentype/spec/cvt)、[maxp](https://learn.microsoft.com/en-us/typography/opentype/spec/maxp)。
