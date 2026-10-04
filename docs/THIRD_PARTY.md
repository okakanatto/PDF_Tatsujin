# ライセンスと第三者部品

自作のアプリケーションコードと開発スクリプトはルートの[MIT License](../LICENSE)で公開します。リポジトリ全体にある第三者部品をMITへ変更するものではありません。

| 部品 | ライセンス・扱い | 固定情報 |
|---|---|---|
| PDF4QT | MIT、サブモジュール内のLICENSEを維持 | `dependency-lock.json` / `.gitmodules` |
| Tesseract / tessdata_best | Apache-2.0 | vcpkgコミット、モデルコミットとSHA-256 |
| Noto Sans JP | SIL OFL 1.1、埋め込み可 | フォントコミットとSHA-256、`licenses/NotoSansJP-OFL.txt` |
| Liberation（PDF4QTに含まれるフォント） | SIL OFL、上流通知を維持 | `licenses/Liberation-fonts.txt` |
| Qt 6.9.3 | 動的リンクのLGPL-3.0、部品ごとの通知あり | `licenses/Qt`、対応ソースアーカイブのハッシュ |
| その他のネイティブ依存 | 個別ライセンス | 固定vcpkgの`share/*/copyright`を配布時にコピー |
| 合成試験文書 | オリジナルの文字・図はCC0-1.0、埋め込みフォントは元のライセンス | `fixtures/manifest.json` |

モデル・フォント・ビルド依存はGit履歴に含めず、固定参照から開発環境へ取得します。実行時には必要な資産をローカルに同梱します。

Windowsバイナリを配布する場合は`licenses/NOTICE.txt`と各部品の通知を同梱し、Qtの対応ソースを同時に提供してください。`scripts/package.ps1`は通知と実行ファイル群をまとめますが、Qtソースを自動取得する処理ではありません。Qtの対象はqtbase、qtsvg、qtimageformats 6.9.3です。取得先は `https://download.qt.io/archive/qt/6.9/6.9.3/submodules/`、検証用ハッシュは`dependency-lock.json`にあります。

MSVCランタイムはMicrosoftの再頒布条件が別途適用されるバイナリです。MITのアプリコード公開を、すべての同梱部品の無条件の再配布許諾と解釈しないでください。現時点ではソースを公開し、正式なバイナリリリースはM1の未実行試験と配布条件の確認後に判断します。
