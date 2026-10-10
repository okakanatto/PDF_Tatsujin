# 既存本文の固定入力

`line-count/criteria.json` は相対改行の固定PDFをそのまま参照し、2→3→1行の手動置換、字体、行送り、拒否入力を追加操作の実装前に固定したものです。`prepare-line-count-criteria.py` で原本を変更せずバイト一致で再現できます。Meiryo UIの字体プログラムは含みません。

`relative-lines/` は横移動のない一定のTd／TDで改行する横書き2行の正例と、横移動・不均一な3行の拒否入力です。`prepare-relative-line-fixtures.py` で各PDF・原本SHA・期待文字・書体・座標を実装前に固定しました。Windows上でtd.pdfとTD.pdfが同名となった最初の生成失敗は、別の非公開診断フォルダとログへ保持し、固有のファイル名で生成し直しました。元の正解を変更しません。

`multiline/` は同一字体・一定の行送りを持つ日英2行編集と、途中で文字サイズを変える拒否入力です。`multiline-rotated/` は90度回転・CropBox・UserUnit=2の別の正例です。いずれも当該操作前に文字・行数・座標・保つものを固定しました。生成手順は `prepare-multiline-text-fixtures.py` と `prepare-rotated-multiline-fixture.py` です。元の試験入力を更新しません。

`font-styles.json` は字体拡張前に固定した日英置換と書体の条件です。`font-state/` は変更対象の直後に、Tfを省略して元のHelveticaを継承する別ブロックを追加した入力です。字体変更による後続ブロックへの状態漏れ、新しい日文文字、埋込み、保存後の再編集を検査します。元の入力を変更しません。生成手順は `../../scripts/prepare-font-state-fixture.py`。Meiryo UIの字体プログラムはソースへ収録しません。

文字・図形・フォームは合成データで、実務文書や個人の署名を含みません。独自の文章と図形はCC0-1.0、埋込みNoto書体はSIL OFL-1.1です。書体の通知は `../../licenses/` に保持しています。

`body-text.pdf` は固定した既存画像編集入力へ日英の文字ブロックを追加した最初の入力です。基礎PDFのTr=3を継承し、新しい文字も不可視になることを最初の試験で検出しました。入力とFAILを保持し、不可視文字・OCR層の拒否試験として使用します。

`visible/body-text.pdf` は新しい文字だけにTr=0を明示した別の正例です。原稿の既存不可視文字は保持します。元の入力・正解を置き換えず、新しい操作前に別の判定を固定しました。`rotated/` は90度回転・CropBox・UserUnit=2の追加正例です。各PDFと判定のSHA-256を固定しています。

`ui-criteria.json` は最初のUI操作前に、日英の置換文字、位置・サイズ、範囲外の署名、拒否文字、取消・更新時の非反映を固定したものです。日本語はQtの貼り付け操作を試験し、ネイティブIMEやOSクリップボードの合格とは区別します。

生成元は `../../scripts/prepare-existing-text-fixtures.py`。字体生成用の `../NotoSansJP-fixture.ttf` は既存の `make-fixtures.py` が固定したNotoSansJPからwght=400で生成する開発用ファイルで、通常利用の依存ではありません。実行済み試験は生成済みPDFをそのまま使用します。再生成して判定を合わせないでください。
