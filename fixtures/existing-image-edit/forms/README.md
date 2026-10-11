# 共有Form内部の画像入力

独自の図形・文は合成CC0データで、実務文書を含みません。既存の固定画像入力と埋込みNoto字体の通知・ライセンスを維持します。Microsoft字体を入力へ追加していません。

`shared-forms.pdf` は同じ外側Formを2回描き、それぞれの内側Formが同じ画像を描く入力です。画像の1回だけを変更し、兄弟呼出しとページ直下の既存画像・文字・AcroFormを保持することを検査します。循環、透明グループ、明示クリップは別の拒否入力です。

`criteria.json` の最初の物理座標案はUserUnit=2を取り落としており、最初の試験が失敗しました。この条件と失敗を保持します。`positive-criteria.json` は元の画像のPDF座標・ページボックス・UserUnitからpypdfで算出した別の正例です。最初の正例操作前に固定しました。PDF入力・元の案・画像のPDF座標・許容誤差は変更しません。`ui-criteria.json` は4桁mmの実UI入力、`bbox-criteria.json` は表示枠の境界と枠外0.01ptの条件を、当該操作前に追加固定しています。

生成元は `../../../scripts/prepare-form-image-fixtures.py`、正例条件は `../../../scripts/prepare-form-image-positive.py` です。既存ファイルを上書きせず、新しい出力先を指定してください。生成済み入力と条件のSHA-256をSource checksで固定しています。
