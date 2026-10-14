# 混在1/2バイトCMapの実装前固定契約

2026-10-13 dev42。`REDACTION_MIXED_WIDTH_CMAP_NEXT.md`の限定範囲を、製品コードの変更前に実PDF・入力対応表・正解・位置・命令・閾値として固定した。一般CMap/M1/M3/M5の完成、MIAPまたは4096命令列の描画作用、一般配布の根拠にはしない。

固定保存先は`fixtures/redaction-copy/mixed-cmap-v1`。44 PDF（成功候補19、非反映拒否候補25）とcriteria.jsonの計45ファイル、1,534,985 bytes。criteria SHA-256は`fc20b71f3990ffe4fa468389f0867297ebe0fcf2c8aa261de03c694b8f8af118`。PDF bytesと各Encoding/ToUnicode/contentのSHAはcriteria内に記録する。元字体は既存`partial-text/identity-H.pdf`の埋め込みNotoSansJP subsetを保持し、原入力PDF SHA・元SFNT SHA・元NotoSansJP.ttf SHA・OFL本文SHAをcriteriaに固定する。元入力、正解、過去の失敗、dev35/dev36は変更しない。

成功候補は1バイトのみ、2バイトのみ、同じ数値の異なる長さ（41と0041）、1/2バイトの境界、複数codespace、実際に複数要素へ展開するcidrange/bfrange配列、同一CID/同一Unicodeの別コード、正負Tc、正負TJ調整、単引用符、二重引用符、1バイト32の非空白Unicode、2バイトの空白Unicode、別名字体を持つ別ページ、兄弟Form、注釈N/R/D外観、数値区間とは異なるbyte単位の矩形範囲、コード値0を非0 CID/非NUL Unicodeへ対応させた条件を分ける。初期の有効Tw=0を維持する。コード値0はCID0/Unicode NULと同一視しない。

拒否候補はprefix曖昧性、同じ長さのcodespace重複、範囲外の使用コード、2バイト切断、Encoding/ToUnicodeでの同じコード再定義、片方だけの対応、同一CIDへ異なるUnicode、CID/Unicodeの桁溢れ、UseCMap命令/辞書、未知命令/任意名metadata/stream辞書項目、正負非0 Tw、二重引用符で設定する非0 Tw、空Tj/空TJ/空二重引用符の有効Tw、使用CID0、使用Unicode NUL、byteごとの下限/上限が逆転した範囲である。出力不在だけで拒否PASSにはしない。実Qtの完了・原本/未保存状態/Undo保持と結び付ける。

各コードは（byte_count,value）をkeyとする。Encodingのcode→CID→GIDと、ToUnicodeのcode→Unicodeは独立した明示表を保存する。GIDからUnicodeは導出しない。各資源binding・使用回数・削除対象・元点座標/flags/metrics/命令bytes/複合参照を固定する。文字列bytesからの分解は独立検査器がcodespaceをbyte単位で照合し、各幅とTc/TJ/引用符/Tm/T*の算術から全原点・advanceを再計算する。実装・serializerから正解を採取しない。

入力エンジンの制約を隠さない。41→公と0041→秘を持つ元PDFについて、PDFiumはToUnicodeを混同して「公開 公匿機密亀鶴 文字保持」と抽出し、pypdfも混在長を正しく抽出しない。明示表の正解「公開 秘匿機密亀鶴 文字保持」は変更しない。PDFiumの元char-index boxesは固定コード順による測定geometryでありUnicodeの正解ではない。両エンジンの元抽出結果を各成功候補に記録し、source_external_extraction_is_Unicode_oracle=falseとする。生成途中の初回assert失敗は`mixed-cmap-pre-freeze-failed-r1`と`evidence/dev42-mixed-cmap-source-extraction-pre-freeze-failure.json`に保持する。保存後のIdentity-Hの文字意味は明示表へ照合し、字形/画素は別検査する。

許容CMap envelopeはこの入力で使うcanonical prolog（CIDInit/ProcSet、12 dict begin、begincmap）、既知CIDSystemInfo（Registry/Ordering/Supplement）、CMapName/CMapType/WMode=0、およびcanonical footer。CMapVersionの受入は今回の入力にない。codespaceはbyteごとの矩形範囲であり、先頭byteをまたぐ曖昧さは拒否する。入力の未知metadataを削除して成功扱いにしない。

位置誤差0.5pt以内、PDFium/Popplerを独立に72/96/300dpiで描画し、矩形の既存1px境界外差0、対象外ページ全画素一致を要求する。全到達/未到達FontFile2、GIDと複合閉包、輪郭/flags/metrics/保持命令/checksum、旧Encoding/ToUnicode/字体/content stream、削除文字列、展開streamと生PDF bytesを別々に検査する。正当に別箇所で保持される単字「秘」と削除対象全文字列を混同しない。局所FontFile2の必要閉包と同一元SFNT SHAの最終到達参照unionを区別する。

UIの固定入力は成功index14 `shared-pages-alias.pdf`、拒否index19 `prefix-ambiguous-refused.pdf`（0-based）。独立検査は未実行/失敗を残し、単なるsource inspectionを製品PASSへ加算しない。

Windowsのcase-insensitive pathで`empty-Tj-Tw`と`empty-TJ-Tw`が衝突した暫定入力を、`mixed-cmap-pre-freeze-failed-r2`と暫定source-r1報告に保持する。SHA guardを期待拒否の例外処理外へ移し、最終入力は空Tjと空TJをsingle/arrayの異なるfilenameで保存した。入力全SHAとcasefold uniquenessを確認する前の暫定source-r1は最終固定・合格の証拠に使わない。

最終独立source preflightは`evidence/dev42-independent-mixed-cmap-source-r2.json`。入力44件の全PDF SHA、別経路の対応、raw bytesの分解、文字原点/advanceを照合し、成功19件をPASS_SOURCE_INSPECTION、拒否25件をPASS_SOURCE_REJECTION_INSPECTIONとして記録する。製品受入・拒否のPASSは0件、未実行である。
