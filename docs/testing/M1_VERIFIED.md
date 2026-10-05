# M1の検証概要と再現手順

M1の実動試作として、PDF4QTライブラリ＋Tesseractによる同じ文書ウィンドウの署名・閲覧・日英OCR・通常PDF保存・再編集を実装した。固定したSPEC・TECH・ACCEPTANCE・AGENTSの要件・入力・正解・閾値は変更していない。**環境未実行が残るためM1全体の合格は保留。M2以降には進んでいない。**

## 使える成果物と起動方法

ローカルの最新試用版は `dist/PDFTatsujin-M1-verified/PDFTatsujin.exe`。DLL・plugins・assets・licensesを同じ配置で保持する。日英OCRモデルと日本語フォントを含み、通常利用にPythonの追加導入は不要。自作ソースはMIT、GitHub Releaseへのバイナリ公開は未実施。

## 実際にできたことと修正

Windows実画面で、日本語IMEによる氏名・異体字・固定日付、配置・移動・Undo／Redo、標準保存ダイアログの取消・保存、PDFの再読込後の文字・サイズ・位置の再編集を確認した。同じアプリで日英スキャンPDFへ署名とOCRを適用し、検索、本文のドラッグ選択とOSコピー、OCRのUndo／Redo、保存を確認した。

その実画面試験で、OCRのUndo後に古い「OCR反映済み」の案内が残る不具合を見つけた。Undo後は「元に戻しました。」、Redo後は「やり直しました。」へ更新した。修正後の配布アプリでも、保存済みOCR PDFの署名の移動・Undo／Redo・再保存・検索と案内を実画面で確認した。実IMEから全ページOCRまでの全ネイティブ経路は案内修正直前の版、修正後は当該箇所の実画面再試験として区別し、ローカルの詳細報告に実行物のhashを残している。

## 試験結果

修正後のWindows全自動回帰は52 PASS／0 FAIL。実OCRのUndo／Redo、署名保持、案内文の回帰も含む。実NTFS書込拒否とWindows PDF印刷ドライバも実行した。実画面で保存したPDFはPDFium・Popplerで独立照合し、再編集可能な署名とOCR本文を保持した。Firefoxではヘッドレスで検索・DOM選択・WebDriverのPDF印刷を確認した。

多ページ閲覧は[測定設計](../design/READING_PERFORMANCE.md)に沿い、同じ製品UIをリンクする独立ターゲットで実際に測った。原本・PDF・Undoを変えず、既存の文字・縮小画像キャッシュの上限を守った。初回の終了時メモリの採取誤りを修正し、生データを保持して再測定した。画像PDFの描画待ちとメモリには改善課題があり、軽快さやAcrobat超えを実証したとは扱わない。

## 未実行・制約

Reader、Firefoxネイティブ検索・OSコピー・印刷ダイアログ、実プリンター、実容量不足、クリーンWindows＋通信無効、Microsoft IME、実OS倍率、Narrator、実画面FPS・入力遅延、時間単位のメモリ試験、Acrobat比較は**未実行**。[再現手順](REMAINING_MANUAL.md)を保持する。以前のFirefoxの連続しおり操作における[Fit診断FAIL](M1_EXTERNAL_FIT.md)も維持し、現在の検索合格で置き換えない。

## 採用構成と再現

構成Aを維持した。共通文書と履歴はDocument／Window、本文表示はCanvas、検索はSearchSession、選択文字はSelectionTextCache、ページ縮小画像はPagePreviewsが担当する。固定した上流ソースは改変していない。測定コードは独立した任意ビルドのターゲットで、製品画面に測定操作を追加しない。

固定環境は[開発手順](../DEVELOPMENT.md)。新しい出力名を指定して実行する。

```powershell
& scripts/build.ps1 -Target PDFTatsujinReadingBenchmark
& scripts/build.ps1
& scripts/package.ps1 -OutputDirectory dist/new-M1 -TestSupport
$env:TATSU_UI_REVIEW='1'
& scripts/test-windows-errors.ps1 -IncludeNativePrinter -AppDirectory dist/new-M1 -OutputDirectory evidence/new-regression
python scripts/evaluate.py evidence/new-regression
python scripts/benchmark-reading.py --harness build/app/bin/PDFTatsujinReadingBenchmark.exe --app-directory dist/new-M1 --output evidence/new-reading
```

公開GitHubにはソース、再現手順、この概要を置く。今回の詳細な環境・画像・測定データはローカル `evidence/native-refined-20261005` と配布フォルダ内の `docs/testing/M1_VERIFIED.md` に保持し、公開していない。ローカル詳細報告にA01〜A12、版ごとの実画面操作、失敗、独立評価、部品別容量と個別測定値を記録した。検証済み範囲を判断する際は環境未実行を含むM1全体の合格と区別する。
