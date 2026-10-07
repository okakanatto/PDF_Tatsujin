param([string]$OutputDirectory = 'dist/PDFTatsujin-M1', [switch]$TestSupport)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Set-Location $root
$compilerConfig=Get-Content -LiteralPath (Join-Path $root 'build/app/CMakeCache.txt') -Raw
if($compilerConfig -match 'TATSU_COMPILER_TEST_DELAY_MS:STRING=[1-9]' -or $compilerConfig -match 'TATSU_FIX_COMPILER_STARTUP:BOOL=OFF'){
    throw 'Compiler fault-injection build cannot be packaged. Run build.ps1 with the normal defaults first.'
}
$out=Join-Path $root $OutputDirectory
New-Item -ItemType Directory -Force $out,(Join-Path $out 'licenses') | Out-Null
Copy-Item -LiteralPath 'build/app/bin/PDFTatsujin.exe' -Destination $out
$core=Get-ChildItem build/app -Recurse -File -Filter Pdf4QtLibCore.dll | Select-Object -First 1
if(!$core){throw 'Pdf4QtLibCore.dll missing'}
Copy-Item -LiteralPath $core.FullName -Destination $out
$widgets=Get-ChildItem build/app -Recurse -File -Filter Pdf4QtLibWidgets.dll | Select-Object -First 1
if(!$widgets){throw 'Pdf4QtLibWidgets.dll missing'}
Copy-Item -LiteralPath $widgets.FullName -Destination $out
Get-ChildItem tools/vcpkg/installed/x64-windows/bin -Filter '*.dll' | Copy-Item -Destination $out
& tools/Qt/6.9.3/msvc2022_64/bin/windeployqt.exe --release --no-translations --no-opengl-sw --no-compiler-runtime (Join-Path $out 'PDFTatsujin.exe')
if($LASTEXITCODE -ne 0){throw 'windeployqt failed'}
if($TestSupport){
    Copy-Item tools/Qt/6.9.3/msvc2022_64/plugins/platforms/qoffscreen.dll (Join-Path $out platforms)
}
# PDF4QT's private dependencies may not all be discovered from the entry executable.
foreach($module in @('Core','Gui','Widgets','Xml','Svg','Test','Concurrent','PrintSupport')) { Copy-Item "tools/Qt/6.9.3/msvc2022_64/bin/Qt6$module.dll" $out }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ runtime location not found'}
$redist=Join-Path $vs 'VC/Redist/MSVC'
$eligibleCrt = { $_.FullName -match '\\x64\\' -and $_.FullName -notmatch '\\(onecore|debug_nonredist)\\' }
$crt=Get-ChildItem $redist -Recurse -Directory -Filter Microsoft.VC145.CRT | Where-Object $eligibleCrt | Sort-Object FullName -Descending | Select-Object -First 1
if(!$crt){$crt=Get-ChildItem $redist -Recurse -Directory -Filter 'Microsoft.VC*.CRT' | Where-Object $eligibleCrt | Sort-Object FullName -Descending | Select-Object -First 1}
if(!$crt){throw 'Desktop x64 Release CRT missing; do not package OneCore or debug runtimes'}
$crtFiles=Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll'
$crtFiles | Copy-Item -Destination $out
[ordered]@{family=$crt.Name;version=$crt.Parent.Parent.Name;configuration='Desktop x64 Release';files=@($crtFiles | ForEach-Object {[ordered]@{file=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLower()}})} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'runtime-origin.json') -Encoding utf8
Copy-Item assets -Destination $out -Recurse -Force
Copy-Item licenses/* -Destination (Join-Path $out licenses) -Recurse -Force
Copy-Item docs -Destination $out -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $out 'examples') | Out-Null
foreach($sample in @('D01.pdf','D03.pdf','D07.pdf')){Copy-Item -LiteralPath (Join-Path $root ('fixtures/'+$sample)) -Destination (Join-Path $out 'examples')}
'D01: Japanese/English digital PDF. D03: Japanese/English image PDF (8 pages). D07: standard AcroForm. Synthetic test inputs, not private documents; original text and graphics are CC0-1.0; embedded font notices are in licenses. See docs/THIRD_PARTY.md.' | Set-Content -LiteralPath (Join-Path $out 'examples/README.txt') -Encoding utf8
Copy-Item vendor/PDF4QT/LICENSE -Destination (Join-Path $out 'licenses/PDF4QT-MIT.txt')
Get-ChildItem tools/vcpkg/installed/x64-windows/share -Directory | ForEach-Object { $copyright=Join-Path $_.FullName copyright; if(Test-Path $copyright){Copy-Item $copyright (Join-Path $out ('licenses/'+$_.Name+'.txt'))} }
if(Test-Path README.md){Copy-Item README.md $out}
foreach($metadata in @('LICENSE','M1_REPORT.md','M3_REPORT.md','M3_RC3_REPORT.md','M3_RC4_REPORT.md','ROADMAP.md','dependency-lock.json')){if(Test-Path $metadata){Copy-Item $metadata $out}}
Get-ChildItem -LiteralPath $out -Recurse -File | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash | ConvertTo-Json | Set-Content evidence/distribution-hashes.json
& scripts/measure-storage.ps1 | Set-Content evidence/storage.json
