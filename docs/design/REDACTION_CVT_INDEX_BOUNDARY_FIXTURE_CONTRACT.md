# dev41 CVT番号境界の固定入力契約

2026-10-13。`REDACTION_CVT_INDEX_BOUNDARIES.md`の次試験案を、新規入力群へ固定した記録。製品実行前に固定し、既存入力・正解・閾値を変更していない。この文書だけを製品試験のPASSとは扱わない。

保存先は`fixtures/redaction-copy/cvt-index-boundaries-v1`。10条件（成功期待9、非反映拒否期待1）、PDF12件とcriteriaの13ファイル、合計1,042,881 bytes。criteria SHA-256は`d8243815eeec4a579639502d3d66533ba876bec483fdc6e3edc813d83453c596`。生成器は`scripts/prepare-redaction-cvt-index-boundaries.py`、既存4入力/criteriaのSHAを先に照合し、全ファイルの再生成byte一致を確認した。

初回暫定criteriaの共有先boxesに元の句の位置が残っている誤りを製品実行前に発見した。暫定criteria・生成器・試験登録・本契約の当時bytesを`evidence/dev41-preexecution-truth-draft-r1.tar.xz`へ全SHA読戻し付きで保持した。`evidence/dev41-preexecution-truth-draft-r1.json`に非実行と誤りを記録。生成済み入力からPDFiumで保持語の位置を固定し直し、PDF12件は全byte不変を確認した。製品結果に合わせて正解を修正したものではない。全体の旧閾値は不変。

基礎4条件は元CVT32767、符号付きPUSHW -32768、密番号255、密番号256。負32768入力は32769要素でunsigned32768なら表内であることを固定し、符号による拒否を範囲外拒否で代用しない。元32767は`01 b0 00 b8 7f ff 3f 31`からCVTだけを密0へ書き換える。主字形は公（元GID1、61実点、先頭on-curve）を257点へ増やし、元座標・flags・contour終点・bbox・advanceを記録した。生成前に元実点≤257と先頭flag&1をassertし、増加分だけcontour終点を移動する。主字形の元1542 bytes→1286 bytes、点256のPUSHWは不変。密256では同じ保持字体の開（元GID2）の元CVT513だけを密256へ写す。

複合子と必要GID0子の2派生も同じ密256条件を持つ。複合自身の命令は空であり、必要子の輪郭とCVT参照を保持する。共有4派生は別ページ、別名字体、兄弟Form、注釈N/R/DとOn/Offを使う。第1ページの公だけが元257..512を参照し、共有先の秘だけが元513を参照する。各局所FontFile2の期待密番号を独立に保存した。前者256要素、後者1要素の513→密0であり、最終同一入力SFNT SHAのwitness union257要素から密256を流用しない。

各source_font_glyphsに元入力SFNT SHA、基礎SFNT SHA、元/ヒントGID、点数、輪郭情報、元命令、元CVT exact bytesを保存。expected_local_closuresは実際の資源bindingキー、期待root/複合閉包、元番号→密番号、exact CVT bytes、glyph別exact命令を固定する。expected_source_reference_unionのsource_font_sha256は製品が読む生成済み入力SFNTのSHAであり、source_font_glyphs.source_font_sha256の基礎字体SHAと区別する。

元32767と密256には対象MIAPだけを除いた描画対照を固定した。点/CVT PUSHとMIAPだけを外し、同じ軸/IUP、他glyph・輪郭・CVT・設定を保つ。密256の対照対象は伴う開のみ。作用が観測できないエンジンは未実証として残し、他glyphの描画差を根拠にしない。

位置0.5pt、PDFium/Poppler72/96/300dpiの既存1px矩形境界外差0、対象外ページ全画素一致を維持する。元CVT全体、未参照marker、除去専用参照90と私的marker、旧字体/対応表の不在、全FontFile2/GID閉包、checksum/head/hhea/maxp、取消・拒否・未保存署名・Undo/Redo・再編集は別担当の独立検査と実Qt記録を必要とする。限定境界試験を一般CVT・一般TrueType・M1/M3/M5全体の完成へ広げない。

