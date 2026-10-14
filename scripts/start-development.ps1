param(
    [string]$Executable = 'build/app/bin/PDFTatsujin.exe',
    [string]$RuntimeDirectory = 'dist/PDFTatsujin-0.5.0-dev36-working'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$binary = [IO.Path]::GetFullPath((Join-Path $root $Executable))
$runtime = [IO.Path]::GetFullPath((Join-Path $root $RuntimeDirectory))
foreach ($path in @($binary, (Join-Path $runtime 'platforms/qwindows.dll'), (Join-Path $runtime 'assets/fonts/TatsujinSansJP-Regular.ttf'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing development runtime file: $path" }
}
$previousPath = $env:PATH
$previousAssets = $env:TATSU_ASSETS
$previousPlugins = $env:QT_PLUGIN_PATH
$previousPlatform = $env:QT_QPA_PLATFORM
try {
    $env:PATH = $runtime + ';' + $env:PATH
    $env:TATSU_ASSETS = Join-Path $runtime 'assets'
    $env:QT_PLUGIN_PATH = $runtime
    Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    # Explicit interactive launcher; no development runtime is copied.
    Start-Process -FilePath $binary -WorkingDirectory $root
} finally {
    $env:PATH = $previousPath
    $env:TATSU_ASSETS = $previousAssets
    $env:QT_PLUGIN_PATH = $previousPlugins
    $env:QT_QPA_PLATFORM = $previousPlatform
}
