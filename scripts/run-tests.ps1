param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$OutputDirectory){$OutputDirectory=Join-Path $root ('evidence/run-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new output directory to preserve earlier evidence.'}
$exe=Join-Path $root 'dist/PDFTatsujin-M1/PDFTatsujin.exe'
$fixtures=Join-Path $root 'fixtures'
$previousPath=$env:PATH
$previousAssets=$env:TATSU_ASSETS
$previousPlugins=$env:QT_PLUGIN_PATH
try {
    $env:PATH="$env:SystemRoot\System32"
    Remove-Item Env:TATSU_ASSETS -ErrorAction SilentlyContinue
    Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    $arguments=@('--selftest',('"'+$fixtures+'"'),('"'+$OutputDirectory+'"'))
    $process=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    Get-Content -LiteralPath (Join-Path $OutputDirectory 'selftest.json')
    if($process.ExitCode -ne 0){throw "Selftest failed with exit code $($process.ExitCode). Evidence: $OutputDirectory"}
} finally {
    $env:PATH=$previousPath
    $env:TATSU_ASSETS=$previousAssets
    $env:QT_PLUGIN_PATH=$previousPlugins
}
