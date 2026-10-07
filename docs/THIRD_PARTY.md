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
| 実スキャン R01/R02 | Commons掲載のパブリックドメイン原稿・機械的スキャン。自作コードのMITとは別扱い | `fixtures/real-scans/manifest.json` の出典、条件、画像・PDFハッシュ |

モデル・フォント・ビルド依存はGit履歴に含めず、固定参照から開発環境へ取得します。実行時には必要な資産をローカルに同梱します。

RC5の書体選択では、Windowsにインストール済みのMeiryo UI、Meiryo、游ゴシック・明朝、MSゴシック・明朝、Arial、Times New Romanを、そのPCで利用できる場合に選べます。Windowsのフォントファイルをアプリのassets・配布ZIP・公開リポジトリへコピーしません。PDFへの文書埋め込みは、実際に使う書体のOpenType `OS/2.fsType` を調べ、再編集とサブセット埋め込みに適合する場合に限ります。権利情報不明、埋め込み禁止、表示・印刷のみ、サブセット禁止、ビットマップのみは拒否します。このPCの上記Windows書体はEditable embedding（8）、同梱NotoはInstallable embedding（0）でした。保存後のサブセットにも元のフラグを保持することを検査します。

条件の確認元：[Microsoftのフォント再配布FAQ](https://learn.microsoft.com/en-us/typography/fonts/font-faq)、[OpenTypeのfsType](https://learn.microsoft.com/en-us/typography/opentype/spec/os2#fstype)。独自に入手した他のフォントの利用条件までこの確認で保証するものではありません。保存済み書体がないPCでは外観を保持し、変更前に別書体の明示選択を求めます。フォント本体の配布を伴う変更は別途条件を確認します。

Windowsバイナリを配布する場合は`licenses/NOTICE.txt`と各部品の通知を同梱し、Qtの対応ソースを同時に提供してください。`scripts/package.ps1`は通知と実行ファイル群をまとめますが、Qtソースを自動取得する処理ではありません。Qtの対象はqtbase、qtsvg、qtimageformats 6.9.3です。取得先は `https://download.qt.io/archive/qt/6.9/6.9.3/submodules/`、検証用ハッシュは`dependency-lock.json`にあります。

MSVCランタイムはMicrosoftの再頒布条件が別途適用されるバイナリです。RC4の梱包ではデスクトップx64 ReleaseのCRTを明示し、OneCore・debug_nonredistを除外します。元の再頒布フォルダとの全DLLのSHA-256一致を検査し、`runtime-origin.json`と`licenses/MSVC-RUNTIME-NOTICE.txt`へ記録します。[Microsoftの配布リスト](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)は有効なVisual Studioライセンスを条件としています。ライセンス保有の確認はPCへのBuild Tools導入確認とは別です。

MITのアプリコード公開を、すべての同梱部品の無条件の再配布許諾と解釈しないでください。RC4のバイナリはローカルの評価用ZIPとして準備し、公開前に配布主体のMSVCライセンス条件を確認します。正式な一般配布はクリーンWindows等の未実行試験も含めて判断します。候補の品質試験はこの確認と並行して進めます。
