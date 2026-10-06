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
$crt=Get-ChildItem $redist -Recurse -Directory -Filter Microsoft.VC145.CRT | Where-Object FullName -Match 'x64' | Select-Object -First 1
if(!$crt){$crt=Get-ChildItem $redist -Recurse -Directory -Filter 'Microsoft.VC*.CRT' | Where-Object FullName -Match 'x64' | Select-Object -First 1}
if($crt){Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $out}
Copy-Item assets -Destination $out -Recurse -Force
Copy-Item licenses/* -Destination (Join-Path $out licenses) -Recurse -Force
Copy-Item docs -Destination $out -Recurse -Force
Copy-Item vendor/PDF4QT/LICENSE -Destination (Join-Path $out 'licenses/PDF4QT-MIT.txt')
Get-ChildItem tools/vcpkg/installed/x64-windows/share -Directory | ForEach-Object { $copyright=Join-Path $_.FullName copyright; if(Test-Path $copyright){Copy-Item $copyright (Join-Path $out ('licenses/'+$_.Name+'.txt'))} }
if(Test-Path README.md){Copy-Item README.md $out}
foreach($metadata in @('LICENSE','M1_REPORT.md','dependency-lock.json')){if(Test-Path $metadata){Copy-Item $metadata $out}}
Get-ChildItem -LiteralPath $out -Recurse -File | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash | ConvertTo-Json | Set-Content evidence/distribution-hashes.json
& scripts/measure-storage.ps1 | Set-Content evidence/storage.json
