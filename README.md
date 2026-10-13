# PDF達人 / PDFTatsujin

PDFを読み、書き込みや署名を加え、日英スキャンを検索できるPDFにするWindowsデスクトップアプリです。文書処理とOCRはローカルで行います。

現在は開発版です。正式なバイナリリリースはまだありません。自作コードは[MIT](LICENSE)、依存部品には各ライセンスが適用されます。

## できること

- 閲覧・検索・文字選択・コピー・印刷
- 日本語テキスト署名の配置・移動・変更
- ページ回転、Undo／Redo、別名保存
- 日本語・英語OCR、対象ページ指定、進捗・取消
- 通常PDFへの保存と、追加した署名の再編集

## 起動と使い方

[Windows開発手順](docs/DEVELOPMENT.md)でビルドしてください。配布フォルダを作成した場合は、フォルダ一式を保持して `PDFTatsujin.exe` を起動します。DLL、plugins、assets、licensesも必要です。日本語フォントと日英OCRモデルを同梱する構成で、通常利用にPythonやTesseractの別途インストールは不要です。

1. 「PDFを開く」で文書を選びます。
2. 「署名」で文字を入力し、ページをクリックして配置します。枠をドラッグして移動できます。
3. 「OCR」で言語と対象ページを選び、処理を開始します。
4. 検索・コピーで結果を確認し、「別名保存」でPDFへ保存します。

合成の試験入力として `fixtures/D01.pdf`（署名）、`fixtures/D03.pdf`（日英スキャン）、`fixtures/D05.pdf`（画像・既存文字・注釈の混在）を用意しています。

## 対応範囲と制約

OCRには誤認識が残ります。複雑な段組みや低品質画像など、任意の文書に対する認識精度は保証していません。文字・画像による署名は、本人性を証明する証明書署名とは別の機能です。矩形や画像を重ねる操作は墨消しにはなりません。

Windowsの自動試験と別エンジンによる保存PDFの検証を実施しています。クリーンなWindows、日本語IME、外部ビューア、印刷、初見操作などは、版ごとに未評価の項目があります。[試験結果](docs/testing/M1_VERIFIED.md)と[対応機能の仕様](SPEC.md)を参照してください。

## 開発

```powershell
git clone --recurse-submodules https://github.com/okakanatto/PDF_Tatsujin.git
```

[構成](docs/ARCHITECTURE.md) · [ビルドと試験](docs/DEVELOPMENT.md) · [貢献方法](CONTRIBUTING.md) · [ロードマップ](ROADMAP.md) · [依存部品のライセンス](docs/THIRD_PARTY.md)
