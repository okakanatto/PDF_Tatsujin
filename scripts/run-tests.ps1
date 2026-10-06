param([string]$OutputDirectory, [switch]$Headless, [string]$AppDirectory = 'dist/PDFTatsujin-M1')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$OutputDirectory){$OutputDirectory=Join-Path $root ('evidence/run-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new output directory to preserve earlier evidence.'}
$exe=Join-Path (Join-Path $root $AppDirectory) 'PDFTatsujin.exe'
$fixtures=Join-Path $root 'fixtures'
$previousPath=$env:PATH
$previousAssets=$env:TATSU_ASSETS
$previousPlugins=$env:QT_PLUGIN_PATH
$previousPlatform=$env:QT_QPA_PLATFORM
try {
    $env:PATH="$env:SystemRoot\System32"
    Remove-Item Env:TATSU_ASSETS -ErrorAction SilentlyContinue
    Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    if($Headless){$env:QT_QPA_PLATFORM='offscreen'}
    $arguments=@('--selftest',('"'+$fixtures+'"'),('"'+$OutputDirectory+'"'))
    $process=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    [ordered]@{exit_code=$process.ExitCode;exe_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower();completed=($process.ExitCode -eq 0);headless=[bool]$Headless;filter=$env:TATSU_TEST_FILTER} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'process-result.json')
    if(Test-Path -LiteralPath (Join-Path $OutputDirectory 'selftest.json')){Get-Content -LiteralPath (Join-Path $OutputDirectory 'selftest.json')}
    if($process.ExitCode -ne 0){throw "Selftest failed with exit code $($process.ExitCode). Evidence: $OutputDirectory"}
} finally {
    $env:PATH=$previousPath
    $env:TATSU_ASSETS=$previousAssets
    $env:QT_PLUGIN_PATH=$previousPlugins
    $env:QT_QPA_PLATFORM=$previousPlatform
}
