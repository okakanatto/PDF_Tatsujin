# M1 検索表示・保存競合の追加修正

> 後続の[署名・操作取消・読込エラーの修正](M1_INTERACTION.md)で配布物を更新し、全27件の回帰試験を実施した。本書の実行結果・ハッシュは、その前の版の記録。

2026-10-04。Windows 11 Home 25H2 x64、Qt 6.9.3。環境不足の試験を記録して分離し、このPCで進められるM1の不具合修正・ビルド・再試験を実施した。要件・入力・正解・検索語・合格閾値は変更していない。**M1全体の合格は保留、M2には進んでいない。**

## 使える成果物と起動

ローカルの `dist/PDFTatsujin-M1-review/PDFTatsujin.exe` を起動する。ZIPは `dist/PDFTatsujin-M1-review-windows-x64.zip`。DLL・モデル・フォントを含むフォルダ全体が必要。操作方法は[README](../../README.md)、ビルド環境は[開発手順](../DEVELOPMENT.md)。GitHubにはソースと合成試験の証拠を登録し、正式なバイナリリリースは作っていない。

試験済みexeのSHA-256: `b5da72ba09a720ec774102cdd4a12dd628f09ece4739ef3cb5ee5f7cda4228c3`。

プロジェクト全体は約4.81GB／許可20GB。配布ZIP約138.7MB、実行フォルダ約145.3MB、Qt対応ソース約53.9MB。実行フォルダの内訳は本体・PDF4QT約9.43MB、Qt約33.11MB、他DLL・ランタイム約45.08MB、OCRモデル約44.06MB、フォント約9.59MB、通知・資料約4.04MB。論理ファイル長で測定し、共有の既存開発ランタイムは含めない。詳細は `evidence/m1-corrections/storage-breakdown.json`。

## 実際に修正したこと

| 問題 | 修正後の動作 | 修正前の実行結果 | 修正後の試験 |
|---|---|---|---|
| 検索語を一致しない語へ変えても、前の強調表示が残る | 語の編集・クリアで強調と件数表示を消す。繰り返し検索で重ならず、文書表示の更新時は確定した検索語の強調を復元 | [FAILの証拠](corrections-before-search.json) | `A01_search_highlight_lifecycle` PASS。実ウィジェットの画像を比較 |
| Windowsで保存先の大小文字表記を変えると、外部変更の検出を回避できる | 既知の原本・保存先を正規化したパスで照合し、読込時／前回保存時のハッシュを維持。原本への上書き確認にも同じ判定を適用 | [FAILの証拠](corrections-before-save.json) | `A10_save_conflict_path_case` PASS。原本と保存済み出力の2経路で外部変更・未保存編集を保持 |

保存試験はテスト専用一時ファイルだけを外部変更し、拒否後に元のバイト列へ戻した場合は保存できること、その後に元の大小文字表記へ戻して保存できることも確認した。ハードリンク等すべての同一ファイル別名を網羅した試験ではない。

検索の状態はCanvasが管理し、保存競合の基準はDocumentが管理する。UIから保存時点のハッシュを受け取っても、既知の保存先ではDocumentの基準を優先する。PDFの保存形式、署名メタデータ、OCRエンジンは変更していない。

## ビルド・実行・試験結果

Windowsでビルド成功、配布フォルダの実行ファイルで **22 PASS／0 FAIL**。[自動試験結果](corrections-regression.json)。既存の20件に上記2件を追加した。署名・移動・Undo／Redo・保存後の再編集、8ページの日英OCR、同一画面の検索・コピー・保存、混在・回転、OCR取消・強制終了、保存失敗、保護PDF、フォーム等の保持、印刷経路を含む。

Qt offscreenで実ウィジェットとPDF処理を動かし、入力はQt QTestで合成した。実際のOSキー入力・IME・OSクリップボードを今回再試験したという意味ではない。前回の[ネイティブ操作結果](NATIVE_UI.md)とは実行版と証拠を区別する。

さらに今回生成したPDFをPDFium・Poppler・pypdfで検証した。[独立検証結果](corrections-independent.json)。

| 測定 | 実結果 | 既定の基準 |
|---|---:|---:|
| 評価用日本語のコピーCER | 0.5051% | 2%以下 |
| 評価用英語のコピーCER | 0.04554% | 1%以下 |
| 固定語の検索 | 日英各20/20 | 各19/20以上 |
| OCR行位置の最大ずれ | 0.9217mm | 2mm以下 |
| 90度回転後の最大ずれ | 0.7964mm | 2mm以下 |
| OCR前後の可視ピクセル差 | 日英8ページ・混在文書とも0 | 外観保持 |
| フォーム6値・コメント・リンク・しおり | 保存前後一致 | 内容保持 |

実行したコマンド（`python`はローカルの検証用ランタイム、依存は開発手順を参照）:

```powershell
& scripts/build.ps1
& scripts/package.ps1 -OutputDirectory dist/PDFTatsujin-M1-review -TestSupport
$env:TATSU_UI_REVIEW='1'
& scripts/run-tests.ps1 -Headless -AppDirectory dist/PDFTatsujin-M1-review -OutputDirectory evidence/m1-corrections/regression
python scripts/evaluate.py evidence/m1-corrections/regression
python scripts/check-source.py
```

19個の固定fixture、正解、Python構文、PDF4QT固定版の検査と、変更したC++のclang-format検査も成功。ビルド・再現・再試験・容量・梱包ログはローカルの `evidence/m1-corrections/`。再実行時は既存証拠を上書きせず新しい出力先を指定する。

## 未実行・制約と次の判断

Adobe Reader、Firefoxのネイティブ検索バー・OSクリップボード・印刷ダイアログ、物理印刷、実容量不足、クリーンWindowsで通信を無効にしたA12、実OS表示倍率100／150／200%比較は**未実行**。前回までのFirefoxヘッドレス検証やWindows PDFプリンター出力を、その代替合格にはしない。[残る試験と手順](REMAINING_MANUAL.md)。

Qtの独自UI＋PDF4QTライブラリ＋ローカルTesseract日英OCRの採用構成を維持する。既に実証した表示・編集・保存を使いながら、画面構成と文書状態の責務を分けて改善できるため。別PCや仮想環境の準備を通常の開発進行の前提にせず、環境制約の判定と実装の進捗を分ける。M1の実施可能な修正をここまで反映し、未実行項目を明示して報告する。M2の機能追加は開始しない。
