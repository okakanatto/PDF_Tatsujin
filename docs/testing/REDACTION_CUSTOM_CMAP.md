# 固定2バイトCMapの墨消し検証

0.5.0-dev27、Windows11 x64／Qt6.9.3／MSVC19.50、Qt offscreen。設計は[REDACTION_CUSTOM_CMAP](../design/REDACTION_CUSTOM_CMAP.md)。

実装前に固定したcustom-cmap-v1は18入力（成功8・拒否10）、criteria SHA044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f。cidchar、cidrange、bfchar、bfrange配列、TJ、共有ページ、引用符、同一CIDの別コードを含む。末尾1バイト、未対応コード、codespace、重複、行数、継承、多文字Unicode、CIDの意味の競合、非ゼロTwと空白、縦書きは拒否する。

追加確認custom-cmap-followup-v1は製品試行前に固定した4入力、SHA463ab0757312ee1ceb00605452bf6369e71f25c1acdd1b15ea8eb1fcfbcb720c。引用符の対象行2条件、Unicode連番、回転90／CropBox／UserUnit1.5付き入れ子Formを扱う。以前の固定条件は変更しない。

M5R18の18条件、M5R19のUI保存・署名保持・別文書再編集・原本Undo保持・拒否時非反映・取消3点、M5R20の追加4条件はPASS。墨消し対象全20名はPASS。Qt抽出に元から空白が挿入される保持行は入力時の行を厳密比較し、固定した保持語・位置は独立検査で維持する。

新しい成功12出力はPDFium／pypdfの対象語不在・保持語、fontTools輪郭一致、元FontFile2／Encoding／ToUnicode／CIDToGIDMap／対象内容の不在、0.5ptとPDFium／Poppler300dpi範囲外差分0を要求し、全てPASS。新12条件の位置差・範囲外差分は0。旧38条件も現行出力でPASS、合計50。空白字形の追加診断1は現行プローブでPASSし、アプリ試験名へ加算しない。

生成はprepare-redaction-custom-cmap.py／prepare-redaction-cmap-followup.py、独立検査はevaluate-redaction-custom-cmap.py。固定ハッシュはC++試験、Python評価、check-sourceで照合する。失敗・未実行・旧exeを保持し、成功出力で上書きしない。ネイティブGUI・OS評価・一般配布・M5全体合格とは区別する。
