# Windows開発手順

Windows 11 x64、MSVC 19.50（Visual Studio Build Tools 2026）、Qt 6.9.3、CMake 4.1.0、Ninja 1.13.0で実際にビルドしました。新規PCで依存をゼロから取得して全手順を通す試験は**未実行**です。以下は固定済みの構成と、既存開発環境で実行したビルド・試験経路です。

## ソースと依存

```powershell
git clone --recurse-submodules https://github.com/okakanatto/PDF_Tatsujin.git
cd PDF_Tatsujin
```

PDF4QTはサブモジュールのコミットで固定しています。`git submodule update --init`は固定版を取得し、`--remote`は使いません。依存の詳細は`dependency-lock.json`にあります。

| 開発用の配置先 | 必要な内容 |
|---|---|
| Visual Studioのインストール先 | C++ Build ToolsとWindows SDK。`vswhere`で検出、必要なら`build.ps1 -VisualStudioPath`で指定 |
| `tools/Qt/6.9.3/msvc2022_64` | Qt 6.9.3のMSVC 2022 x64版。qtbase、qttools、qtsvg、qtspeech、qttranslations、qtimageformats、qtmultimedia |
| `tools/python` | CMake 4.1.0（`cmake/data/bin`）、Ninja 1.13.0（`bin/ninja.exe`）のPythonパッケージ |
| `tools/vcpkg` | `5dd2e1600d049b498ff9fb9fe15997533ae0c804`にcheckoutしたvcpkg |
| `tools/vcpkg/installed/x64-windows` | tbb、openssl、lcms、zlib、openjpeg、freetype、libjpeg-turbo、libpng、blend2d、tesseractと推移依存 |
| `assets` | 次の取得スクリプトで固定ハッシュを検証するフォント・OCRモデル |

vcpkgはclassic mode、triplet `x64-windows`、overlay `vendor/PDF4QT/vcpkg/overlays/general`を使用しました。Qtをvcpkgで重複ビルドする構成ではありません。Qtの取得にはaqtinstall 3.3.0を使用しました。ツール・依存の初回インストール例は次のとおりです。**この取得例の新規PCでの通し実行は未実行**です。

```powershell
python -m pip install --target tools/python cmake==4.1.0 ninja==1.13.0 aqtinstall==3.3.0
$env:PYTHONPATH = Join-Path (Get-Location) 'tools/python'
python -m aqt install-qt windows desktop 6.9.3 win64_msvc2022_64 --outputdir tools/Qt -m qtsvg qtimageformats qtspeech qttranslations qtmultimedia
git clone https://github.com/microsoft/vcpkg.git tools/vcpkg
git -C tools/vcpkg checkout 5dd2e1600d049b498ff9fb9fe15997533ae0c804
& tools/vcpkg/bootstrap-vcpkg.bat -disableMetrics
& tools/vcpkg/vcpkg.exe install tbb openssl lcms zlib openjpeg freetype libjpeg-turbo libpng blend2d tesseract --triplet x64-windows --overlay-ports=vendor/PDF4QT/vcpkg/overlays/general --clean-after-build --binarysource=clear
python scripts/fetch-assets.py
```

依存の取得にはネットワークを使います。アプリでのPDF処理・OCRはローカルです。`fetch-assets.py`は版とSHA-256を固定し、取得完了・検証後にだけ配置します。既存ファイルが変更されている場合は自動上書きしません。既存資産の検証は `python scripts/fetch-assets.py --verify-only` で実行できます。

この開発フォルダには20GBの上限を設定しています。依存構築中の一時ファイルも含め、`scripts/measure-storage.ps1`で確認します。別環境の初回ビルドが常に20GB未満で済むという保証ではありません。

## ビルドとローカル配布フォルダ

```powershell
& scripts/measure-storage.ps1
& scripts/build.ps1 -Upstream
& scripts/build.ps1
& scripts/package.ps1
& dist/PDFTatsujin-M1/PDFTatsujin.exe
```

`-Upstream`は未改変のPDF4QTビューアをビルドする成立性確認用です。通常の変更ではアプリだけをビルドします。`package.ps1`はDLL、プラグイン、モデル、フォント、通知をまとめます。バイナリの外部配布については[第三者部品](THIRD_PARTY.md)も確認してください。

ビルドは日本語パスに起因するツールの不具合を避けるため、既定でT:をプロジェクトへ`subst`します。複製は作りません。T:が別用途で使用中なら停止します。初回から `-Drive U` のように空いているドライブを指定できます。CMakeキャッシュは絶対パスを記録するため、既存ビルド途中で作業ドライブを変えないでください。終了後、不要になった割り当ては `subst T: /d` で外せます。

日本語MSVCの`/showIncludes`出力は、構成時に`cmake/DetectMsvcIncludes.cmake`で実測します。接頭辞を検出できない場合は、ヘッダー依存のないビルドを黙って続けず停止します。追加検証前の古いビルドディレクトリを引き継ぐ場合は、依存情報のなかったオブジェクトを残さないよう一度クリーンビルドしてください。

## M1回帰試験

```powershell
& scripts/run-tests.ps1
```

デスクトップを使わず検証する場合は `package.ps1 -TestSupport` でoffscreenプラグインを含め、`run-tests.ps1 -Headless`を指定できます。これはIMEやWindowsネイティブ画面操作の試験ではありません。追加のNTFS・Windows PDFプリンター・Firefox試験は[M1追加検証](testing/M1_FOLLOWUP.md)に実行手順と制約を記録しています。

パッケージ内のexeでQtウィンドウ、ポインター操作、クリップボード、別プロセスOCR、保存・再読込を試験します。新しい `evidence/run-日時` に結果を保存し、古い結果を上書きしません。`TATSU_ENABLE_SELFTEST`は既定ONです。`build.ps1 -WithoutSelfTests`では試験コードとQtTestリンクを除きますが、M1受入試験にはONでビルドしてください。

独立したPDFium・Poppler・pypdfによる検証にはPythonとPopplerを用意します。試験用Python依存は `scripts/requirements-test.txt` で固定しています。

```powershell
python -m pip install -r scripts/requirements-test.txt
$env:TATSU_POPPLER_BIN = 'C:\path\to\poppler\bin'
python scripts/evaluate.py evidence/run-実際の日時
```

PopplerのbinをPATHへ追加する方法も使えます。これらは開発・検証用で、通常のアプリ起動には不要です。基準のPDFと正解はGitに含まれるため、試験前に再生成しません。生成スクリプトは由来の記録です。基準文書の更新を試験の再実行に混ぜないでください。

## ソース検査とCI

PDF4QTのワーカー起動修正は、固定元ソースからビルド領域の1ファイルを派生して適用する。サブモジュールは改変せず、依存lockとソース検査で元と適用コードを照合する。通常の`build.ps1`は修正ON・遅延0を必ず指定する。

通知喪失の再現には`build.ps1 -CompilerTestDelayMs 500 -WithoutCompilerQueueFix`、修正後比較には`build.ps1 -CompilerTestDelayMs 500`を使い、`TATSU_TEST_FILTER=Reading_compiler_startup`で固定D01の実描画を比較する。この構成は検証専用で、packagerは拒否する。検証後は通常の`build.ps1`へ戻す。OCR入力・正解・描画待ちの10秒条件は変更しない。

多ページ閲覧の応答・メモリ測定は[測定設計](design/READING_PERFORMANCE.md)に沿って独立した任意ターゲットで実行できます。本製品と同じUIライブラリをリンクし、製品exeや通常の画面には測定操作を追加しません。

```powershell
& scripts/build.ps1 -Target PDFTatsujinReadingBenchmark
python scripts/benchmark-reading.py --harness build/app/bin/PDFTatsujinReadingBenchmark.exe --app-directory dist/PDFTatsujin-M1-verified --output evidence/new-reading-measurement
```

psutilを含む固定済みの試験用Python環境を使います。スクリプトは指定した配布フォルダのDLL・Qt offscreen・assetsを利用します。全ページ3周・文書ごと3プロセスの生データ、共通OS時刻による周回待機中のメモリ、実行物とソースのhashを残します。物理ディスプレイFPS、コールド起動、Acrobat比較とは区別します。

```powershell
python -m pip install -r scripts/requirements-format.txt
python scripts/check-source.py
Get-ChildItem src,tests -Recurse -Include *.cpp,*.h | ForEach-Object { clang-format --dry-run --Werror $_.FullName }
```

GitHub Actionsは書式、Python・PowerShell構文、試験文書と正解のハッシュ、PDF4QTの固定コミットを検査します。CIは現時点でWindowsアプリのビルド・GUI試験を実行しません。実行環境の異なるCIの成功をM1合格と解釈しません。

## M2/M3の再現

2026-10-06の評価版は `dist/PDFTatsujin-0.2.0-rc2-windows-x64`。新しい出力先を指定し、過去の証拠を上書きしません。M1の固定入力・閾値は同じまま、B01〜B08の保存・再編集・複合作業も製品のselftestから実行します。

```powershell
& scripts/build.ps1
& scripts/package.ps1 -OutputDirectory dist/PDFTatsujin-0.2.0-rc2-windows-x64 -TestSupport
$env:TATSU_UI_REVIEW = '1'
& scripts/test-windows-errors.ps1 -AppDirectory dist/PDFTatsujin-0.2.0-rc2-windows-x64 -OutputDirectory evidence/new-acceptance -IncludeNativePrinter
python scripts/evaluate.py evidence/new-acceptance
python scripts/evaluate-m2.py evidence/new-acceptance
python scripts/diagnose-real-scans.py --app-directory dist/PDFTatsujin-0.2.0-rc2-windows-x64 --output evidence/new-real-scans
python scripts/measure-candidate.py --app-directory dist/PDFTatsujin-0.2.0-rc2-windows-x64 --output evidence/new-measurements
& scripts/build.ps1 -Target PDFTatsujinReadingBenchmark
python scripts/benchmark-reading.py --harness build/app/bin/PDFTatsujinReadingBenchmark.exe --app-directory dist/PDFTatsujin-0.2.0-rc2-windows-x64 --output evidence/new-reading
```

署名ドラッグ・OCR中の閲覧／取消は、別の任意ターゲットで製品UIをリンクします。製品のexeは変更せず、各3回を記録します。以下を同じPowerShellで実行しました。`Start-Process -Wait`でWin32測定アプリの終了を待ち、終了コードと `interactions.json` の完了を確認してください。

```powershell
& scripts/build.ps1 -Target PDFTatsujinInteractionBenchmark
$candidate = (Resolve-Path dist/PDFTatsujin-0.2.0-rc2-windows-x64).Path
$env:PATH = $candidate + ';' + $env:SystemRoot + '/System32'
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_PLUGIN_PATH = $candidate
$env:TATSU_ASSETS = Join-Path $candidate 'assets'
$inputPath = (Resolve-Path fixtures).Path
$outputPath = Join-Path (Get-Location) 'evidence/new-interactions'
$result = Start-Process -FilePath build/app/bin/PDFTatsujinInteractionBenchmark.exe -ArgumentList @(('"'+$inputPath+'"'), ('"'+$outputPath+'"')) -WindowStyle Hidden -Wait -PassThru
$result.ExitCode
```

Firefoxの外部検証は、既存の開発用Firefox 157.0／geckodriver 0.37.1と `scripts/requirements-viewer-test.txt` のSelenium環境で次を実行しました。別プロファイルのheadless PDF.js検索・選択とWebDriver印刷を検査します。通常アプリの依存ではありません。ネイティブブラウザ操作／OSクリップボードとは区別します。

```powershell
python scripts/test-firefox.py evidence/new-acceptance evidence/new-firefox --firefox tools/viewer-test/firefox/core/firefox.exe --geckodriver tools/viewer-test/geckodriver/geckodriver.exe
python scripts/test-firefox-m2.py evidence/new-acceptance evidence/new-firefox-m2 --firefox tools/viewer-test/firefox/core/firefox.exe --geckodriver tools/viewer-test/geckodriver/geckodriver.exe
```

Pythonの独立評価・計測には固定済み試験依存を使います。通常利用には不要です。全試験時は `TATSU_TEST_FILTER` を解除します。最小画面の追加検査は `TATSU_UI_REVIEW=1`。PowerShellの `process-result.json` の終了コード・完了と `selftest.json` の結果を両方確認します。途中停止した部分結果を全件合格にしません。実NTFS権限拒否試験は自身が作ったフォルダだけを変更し、finallyで元のACLを復元します。実容量不足・クリーンWindows・OS表示倍率・初見評価は別試験です。

## RC3の実行記録

2026-10-07の製品候補は `dist/PDFTatsujin-0.2.0-rc3-windows-x64`。ビルド後のworking配布物で次を実行し、バイナリを変更せず最終候補へコピーしました。再実行は新しい証拠先を使います。通常のビルド設定はRelease・selftest ON・コンパイラー開始修正ON・試験遅延0。新しい画像変換の条件はCMakeの固定ソースhashと `dependency-lock.json` に記録しています。

```powershell
& scripts/build.ps1
& scripts/package.ps1 -OutputDirectory dist/PDFTatsujin-0.2.0-rc3-working -TestSupport
Remove-Item Env:TATSU_TEST_FILTER -ErrorAction SilentlyContinue
$env:TATSU_UI_REVIEW = '1'
& scripts/test-windows-errors.ps1 -AppDirectory dist/PDFTatsujin-0.2.0-rc3-working -OutputDirectory evidence/new-rc3-regression -IncludeNativePrinter
python scripts/evaluate.py evidence/new-rc3-regression
python scripts/evaluate-m2.py evidence/new-rc3-regression
& scripts/test-startup-cleanup.ps1 -AppDirectory dist/PDFTatsujin-0.2.0-rc3-working -OutputDirectory evidence/new-rc3-startup
python scripts/measure-candidate.py --app-directory dist/PDFTatsujin-0.2.0-rc3-working --output evidence/new-rc3-performance
& scripts/build.ps1 -Target PDFTatsujinReadingBenchmark
python scripts/benchmark-reading.py --harness build/app/bin/PDFTatsujinReadingBenchmark.exe --app-directory dist/PDFTatsujin-0.2.0-rc3-working --output evidence/new-rc3-reading
python scripts/compare-reading.py --baseline evidence/previous-reading --candidate evidence/new-rc3-reading --output evidence/new-rc3-comparison.json
```

`benchmark-reading.py` は計測exeだけを出力先のrunnerへコピーし、指定候補のDLL・plugins・assetsを使います。WindowsはPATHよりexeの隣のDLLを優先するため、build/binにexeを置いたままPATHだけ変更する比較は避けてください。比較ツールは固定PDFの同一hash、各3回・3周の完了、同じ製品描画方法、代表画面の全画素一致を要求します。全画面／全条件の一致保証ではありません。RC2の同日基準はRC3のCore DLLをビルドする前に測定し、その時点で実際に読み込むDLLがRC2のものと同一であることを照合しました。

回収試験は自身の新しい作業ディレクトリと、その子に限定した実NTFSジャンクションを作ります。TMP／TEMPは試験子プロセスだけに変更し、終了後に戻します。無関係な領域・リンク先は保持します。OS設定・権限・ネットワークを変更するクリーン／オフライン試験ではありません。

製品版のネイティブ署名・IME・保存・再読込はComputer Useで確認し、Qt offscreenの71件とは別の証拠へ記録しました。GitHub Actionsのソース検査を、これらのWindows動作試験の代わりにはしません。
