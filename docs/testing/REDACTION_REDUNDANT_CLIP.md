# 可視領域を変えない矩形クリップの検証

0.5.0-dev28、Windows11 x64／Qt6.9.3／MSVC19.50、Qt offscreen。設計は[REDACTION_REDUNDANT_CLIP](../design/REDACTION_REDUNDANT_CLIP.md)。

実装前に固定したredundant-clips-v1は21入力、成功13・拒否8、SHAe3d31bbc74fdf0c28830fb746b1b6d29e4c058eca08f0d8940766cf401bd0c46。追加確認redundant-clips-followup-v1は試行前に固定した4入力、成功1・拒否3、SHA81489b5b301af21191ab4103e3425a417fe2da379232e47faf56d48b848d8bc0。元のcurve-refused名は実際にはm／l／hの矩形パスで、実Bezierは追加のbezier-refusedで検査した。条件・閾値を変更しない。

M5R21は25条件、M5R22はUI保存・未保存署名・別文書再編集・原本Undo、拒否時非反映と取消3点を検査してPASS。対象外ページの非包含clipは保持し、共有字体だけを再構築する。独立検査は対象外の同じ語を消えたとしない。対象外ページの300dpi画素差0も確認する。

独立14成功出力をPDFium／pypdfの文字・0.5pt、fontToolsの輪郭、元字体・対応表・対象内容と選択clip構築／W命令の不在、PDFium／Poppler300dpi範囲外差分0で検査してPASS。旧50条件も現行出力でPASS、合計64。空白字形プローブ1は別枠。全体226＋Windows2＝228名PASS、全体1回、旧226名を保持する。

初期対象回帰では既存rotation-90保存名との衝突を上書き拒否で検出した。保存prefixだけをclipへ分離し、旧ファイル・FAIL・初期成功出力を保護した。生成prepare-redaction-redundant-clips／prepare-redaction-clip-followup、評価evaluate-redaction-redundant-clips。初期出力の再現は--output-prefix path、現行出力はclipを使用する。入力PINとC++／check-sourceも照合する。
