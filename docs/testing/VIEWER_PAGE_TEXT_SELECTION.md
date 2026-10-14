# ページ全文選択の実行記録

2026-10-11。製品ソース `6953c68a87df02568132e93da99d360e024045d0`、0.5.0-dev23、exe SHA `afbab907a143934bd8685e9f82765c3f4262e68c1ccb6cd1377c838541b7d666`。Windows 11 25H2、Qt 6.9.3、MSVC Release、Qt offscreen。[操作設計](../design/VIEWER_PAGE_TEXT_SELECTION.md)。

`Selection_page_text` は既存の固定された `viewer-selection-manifest.json` の3ページを使い、各ページの日本語・英語全文を正解と完全一致で比較する。Ctrl+A、右クリックしたページ、Shiftクリック、ズーム後の維持、選択描画のページ範囲、PDFとUndoの非変更がPASS。

`Selection_page_lifecycle` は非同期取消後の遅延コールバック、文書更新、検索・署名・実PDFフォーム欄のCtrl+A/C、回転・CropBox・UserUnit、スキャンのOCR案内、暗号化PDFのコピー禁止を確認した。固定D10の100ページを20部結合した実2000ページPDFで、一覧から1050ページへ移動し全文選択した際の抽出ページ数は2。全ページを先読みしないことがPASS。

| 実行経路 | 結果 |
|---|---|
| 通常全回帰（選択8件と画面配置レビューを含む） | 212 PASS、1実行。以前の210名を保持して上記2名を追加 |
| NTFS書込み拒否／Microsoft Print to PDF | 2 PASS。ACL復元確認。実プリンターではない |
| M1のPDFium／Poppler／pypdf独立検査 | 26 PASS |
| 固定候補／実ZIP展開 | 各22 PASS。同じexe SHA |
| 通信能力ゼロAppContainer | 署名・日英OCR・取消・縦書きの19 PASS。観測18プロセスのcapability=0。新全文選択とJavaの通信制限はこの経路では未実行 |
| ソース検査 | 成功 |

最初はCropBox外の空白だけが全文コピーに残る不具合がFAILとなり、実字形のないページの範囲を空にして修正した。初期表示前のスキャン操作と内部ページ移動の試験準備も、描画待ちと実際の一覧操作へ修正した。正解・入力・権限を変更せず、旧FAILと再試験記録はローカルに保持する。バイナリZIPは153,783,883バイト、SHA `3f52a468603f1714d0273001bc98d4c5215fab70ccc3fe362a67798636b72731`、全617メンバーのCRC／SHAと実614ファイル／613マニフェストを確認した。

OSクリップボード、ネイティブGUI／IME／表示倍率、クリーンWindows、Reader／Word GUI、第三者初見・Acrobat比較、実容量不足、配布主体の再配布条件はこの段階では未実行／未完了。Qtのクリップボードと自動画面試験を、それらの代替合格として扱わない。一般バイナリ公開・全体受入は未宣言。
