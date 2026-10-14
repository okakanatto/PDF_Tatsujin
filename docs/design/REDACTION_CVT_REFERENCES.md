# 墨消し後に残す限定TrueType CVT参照

2026-10-12、dev40。製品実装前に入力・正解・閾値を固定する。既存SPEC/TECH/ACCEPTANCE、hinting-v1、hint-boundaries-v1は変更しない。一般TrueTypeヒント、一般M5、配布候補更新の合格条件ではない。

入力は既存partial-text/visible.pdfおよびshared-fonts-v1のpages/alias-font/sibling-form/appearance-states.pdfの固定SHAを照合して作る。日本語・英語の文字、座標、削除矩形、正解は元criteriaを引き継ぐ。Noto埋込字体の利用条件は維持し、追加の試験内容はCC0合成文書とする。

受入文法は既存の閉じたSVTCA/PUSH実点/MDAP[1]/IUPに、SVTCA→正規PUSH実点→正規PUSH CVT番号→MIAP[1](3f)→IUPを加える。軸はy→x、片軸も許可。各軸の点番号は昇順一意、phantom点を含めない。PUSHB[1]は0..255、PUSHW[1]は256..32767を符号付きで読む。MIAP[0]、NPUSH、複数PUSH、算術、制御フロー、RCVT、CVT書込み、storage、zone変更、fpgm、未知prep、複合字形自身の命令は拒否を維持する。MDAP/MIAP混在は同じ点順序規則で許可する。

元cvtは偶数長のsigned FWORD列。保持字形、必要な複合子、共有字体使用が参照する元番号だけを昇順一意で集めて新cvtを作る。値が同じでも元番号が違えば別要素とする。命令のCVT operandだけを元番号→密番号へcanonical PUSHB/PUSHWで再符号化し、輪郭・点番号・点flag・advance・他命令を保持する。参照番号が表外または負数なら非反映拒否。空・欠落・奇数長cvtのMIAP入力も拒否。

既存hinting-v1/control-values.pdfはMDAPだけ+cvt存在の拒否を固定済み。これと両立する今回の範囲は、非空cvtがあるとき保持/必要複合子/共有閉包内に少なくとも1つMIAP参照がある場合だけ受入する。未参照のみ、または除去字形のみがMIAPを持つ字体は拒否する。値38の特別扱いはしない。成功入力の未参照値・除去専用値は必ず破棄する。

共有閉包は最終候補のpage/Form/N-R-D外観から到達する生成FontFile2を、元字体payloadのSHA256ごとに集約する。局所subsetがMIAPを必要としなければcvtを出力せず、同じ元字体の最終到達subsetにMIAPが残る場合だけ共有条件を満たす。上書きされて到達しない中間subsetや別の元字体の参照を根拠にしない。入力・正解・閾値の固定内容は変えない。

成功入力を以下で固定する。

| profile | 固定内容 |
| --- | --- |
| sparse-byte | 元CVT番号17,7,23,3を使用、昇順3,7,17,23→0,1,2,3 |
| sparse-word | 元CVT番号300,7,256,17を使用、昇順7,17,256,300→0,1,2,3 |
| signed-fwords | 元番号7,17,256,300のFWORD値を-32768,-1,0,32767に固定 |
| equal-fwords | 上記4元番号の値を0に固定、4要素を維持 |
| y-only | y軸のMIAP/IUPだけ、x軸なし |
| mixed-mdap-miap | y軸の点0をMIAP、最後の実点をMDAP、x軸はMIAP |
| removed-private | 保持参照に加え、未使用値と除去専用値に別の連続ASCII marker |
| compound-child | 保持文字の輪郭を追加の非mapped子へ移し、子だけがMIAP |
| required-notdef-child | 同じ輪郭をGID0の必要な子へ移し、子だけがMIAP |
| shared-pages / shared-alias-font / shared-sibling-form / shared-appearance-states | 元の除去単字「秘」のCVT参照が別ページ/別名/兄弟Form/N-R-D外観の必要使用から保持される |

拒否入力はmissing-cvt、empty-cvt、odd-cvt、out-of-range、negative-word-32768、negative-word-65535、noncanonical-word、truncated-word、miap-zero、wcvt、rcvt、compound-own、point-phantom、duplicate-point、reversed-axes、unknown-prep、font-function、storage、zone-change、no-retained-cvt、unused-only-cvt。最後の二つは保持閉包にMIAP参照がない。

全入力の元cvtは320要素を基本とし、通常参照値は対象字形の元座標で固定する。私的markerは`UNREFERENCED_CVT_PRIVATE_40`と`REMOVED_CVT_PRIVATE_40`であり、全到達・未到達・展開ストリームとPDF生バイトから不在を要求する。旧字体、旧対応表、元cvt全体のバイト列も不在を確認する。共有から必要な値が正当に残る場合に、削除単字そのものの全体不在を要求しない。

独立監査は資源binding→GID→保持/複合閉包を元PDFから再構成し、元CVT番号の昇順mapと再符号化命令の正解を製品とは別のコードで求める。全保持輪郭・flags・metrics・命令を比較し、mapped文字だけでなく必要な非mapped子と無参照glyfの不在、全FontFile2の結合を調べる。全字体のcvtは必要値だけのexactバイト列と一致させる。maxStackElementsは実際の最大2、maxSizeOfInstructionsは書換後の最大、その他maxpは輪郭から再計算して照合する。既存空/固定dropout prepだけを許可する。

PDFium/pypdfの位置誤差は既存0.5pt以内、PDFium/Popplerの72/96/300dpiは既存1px矩形境界外の画素差0、対象外ページは全画素一致。低DPI作用の観測は別条件として、sparse-wordのCVT対象字形からMIAP操作だけを除いた対照PDFを生成時に固定する。他字形のヒント、座標、CVT値は同じにし、72/96dpiで対象CVTの描画作用を観測する。MIAP自身の作用が観測されない場合は未実証と報告し、保持成功だけで実証扱いにしない。

製品側の完了した実Qt試験の入力別成功/拒否、出力不在、取消・非反映・未保存署名保持・Undo/Redo・保存後再編集の結果を独立監査で照合する。実行結果ファイルがない単なる出力不在を拒否PASSとはしない。

根拠：[Microsoft TrueType命令仕様](https://learn.microsoft.com/en-us/typography/opentype/spec/tt_instructions)、[cvt](https://learn.microsoft.com/en-us/typography/opentype/spec/cvt)、[maxp](https://learn.microsoft.com/en-us/typography/opentype/spec/maxp)。任意prep/fpgm、CFF、可変字体、一般CMap/DR/DA/共有Formは未完了。
