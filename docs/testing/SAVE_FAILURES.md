# Windowsでの保存異常試験

固定の合成入力を使い、元文書と保存先の保持を検証します。以下の試験コードや手順が存在することを、実行済み・合格の証拠とは扱いません。各実行のJSON、終了コード、入力と実行物のハッシュを別に保存してください。

## 保存先の共有ロック

`PDFTatsujinFilesystemProbe fixtures new-output` は、新規にコピーした合成PDFだけに実際のWindows共有ロックを取得します。原本コピーへの上書きと別名保存先への上書きで、候補検証後の置換失敗を起こします。ファイル・文書・未保存状態・Undo履歴・保存先を照合し、ロック解放後の再試行と再読込を確認します。権限拒否や容量不足の代替試験ではありません。

## 実容量不足

`scripts/test-disk-full.ps1` の既定値 `-Mode Check` は権限と実行環境を確認するだけです。`-Mode Create` は新しい証拠フォルダへ64 MiBのVHDファイルを作成し、接続しません。これらの成功を、容量不足の試験合格とは呼びません。

`-Mode Execute` には管理者PowerShellが必要です。スクリプト自身が新しく作った64 MiBのVHDだけを接続し、ディスクイメージとの対応、サイズ、バス種別、起動／システムディスクでないこと、既存ディスク番号でないことを照合してからNTFSを作成します。利用者が指定した物理ディスク番号やドライブ文字をフォーマットする経路はありません。最後に接続を解除し、解除失敗も記録します。

試験ハーネスも、NTFS、専用ラベル、所有マーカー、8〜64 MiBのボリューム容量を検証します。専用フォルダの実データを増やして空きを64 KiB以下にし、新規・既存の宛先への保存を試します。疑似例外ではなく実際の容量制限です。保存失敗後の状態、原本と既存宛先の保持、一時候補の除去を照合し、充填ファイル解放後にUndo／Redo、保存、再読込、署名の再編集可能性と外観を確認します。ハーネスはAPI試験で、GUIのエラー表示は別に確認します。

このチェックアウトのルートから、存在しない新しい出力フォルダを指定してください。

~~~powershell
scripts/build.ps1 -Target PDFTatsujinFilesystemProbe
scripts/test-disk-full.ps1 -Mode Check -OutputDirectory evidence/capacity-new-check
scripts/test-disk-full.ps1 -Mode Create -OutputDirectory evidence/capacity-new-image
# 管理者PowerShell。製品とハーネスのビルド・配置後に実行する。
scripts/test-disk-full.ps1 -Mode Execute -OutputDirectory evidence/capacity-new-run
~~~

`Execute` の初期化・フォーマット・接続解除までを対象環境で実行していない場合、実容量不足の結果は「未実行」としてください。

## 実プロセス終了による保存中断

`scripts/test-save-interruption.py` は専用ワーカーが使用する通常の `Document::save` を観察します。製品側に保存の待機や故障注入を追加しません。専用プロセスが準備した合成PDFには、可視内容を変えない24 MiBの決定的なストリームを加え、観察の機会を作ります。固定入力は変更しません。

`--boundary candidate-created` と `--boundary candidate-written` を別々に実行できます。後者では候補の全バイトが事前検証したPDFと一致し、保存先をまだ置換していないことを確認します。開始した自身の実行ファイルとPIDを照合してから一時停止・終了し、新規宛先の不在と既存宛先のハッシュ・読み取り可能性を検証します。GUIや利用者の他のプロセスへ入力しません。

~~~powershell
python scripts/test-save-interruption.py --harness build/app/bin/PDFTatsujinFilesystemProbe.exe --app-directory dist/PDFTatsujin-0.2.0-rc10-working --output evidence/save-created-new --boundary candidate-created --check-cleanup
python scripts/test-save-interruption.py --harness build/app/bin/PDFTatsujinFilesystemProbe.exe --app-directory dist/PDFTatsujin-0.2.0-rc10-working --output evidence/save-written-new --boundary candidate-written --check-cleanup
~~~

`--check-cleanup` は実際の終了後に、事前検証した合成PDFを別の保存先へ再保存し、再読込・外観・署名・元の保存先の保持を確認します。この保存は、[所有マーカーとプロセスロックによる整理](../design/SAVE_CANDIDATES.md)も通り、中断した候補だけが除去されたことを照合します。指定しない場合は中断候補を証拠として保持します。

有限の観察で、停電・全タイミング・置換後の異常終了まで保証しません。プロセス終了後の未保存メモリの復旧も対象外です。強制終了直後に残る候補と、次回保存時の整理は、通常の例外時の片付けと区別して記録してください。旧版や識別できない候補は自動除去しません。
