param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$AppDirectory = 'dist/PDFTatsujin-0.2.0-rc3-working'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if (!$output.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)) {
    throw 'Use a new test directory inside this checkout.'
}
$temporaryRoot = Join-Path $output 'tmp'
$outside = Join-Path $output 'outside'
$owned = Join-Path $temporaryRoot 'pdf-tatsujin-job-abandoned'
$unmarked = Join-Path $temporaryRoot 'pdf-tatsujin-job-unmarked'
$wrongMarker = Join-Path $temporaryRoot 'pdf-tatsujin-job-another-app'
$junction = Join-Path $temporaryRoot 'pdf-tatsujin-job-junction'
$nested = Join-Path $temporaryRoot 'pdf-tatsujin-job-nested'
New-Item -ItemType Directory -Path $output,$temporaryRoot,$outside,$owned,$unmarked,$wrongMarker,$nested | Out-Null
foreach ($directory in @($owned,$outside,$nested)) {
    [IO.File]::WriteAllText((Join-Path $directory '.tatsujin-owner'),'PDFTatsujin job v1',[Text.UTF8Encoding]::new($false))
}
[IO.File]::WriteAllText((Join-Path $wrongMarker '.tatsujin-owner'),'Another application',[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $outside 'preserve.txt'),'Test payload; must remain unchanged.',[Text.UTF8Encoding]::new($false))
$expectedHash = (Get-FileHash -LiteralPath (Join-Path $outside 'preserve.txt')).Hash
New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
New-Item -ItemType Junction -Path (Join-Path $nested 'alias') -Target $outside | Out-Null
$exe = Join-Path (Join-Path $root $AppDirectory) 'PDFTatsujin.exe'
$previousTMP = $env:TMP
$previousTEMP = $env:TEMP
$previousPlatform = $env:QT_QPA_PLATFORM
$previousPath = $env:PATH
$previousAssets = $env:TATSU_ASSETS
$previousPlugins = $env:QT_PLUGIN_PATH
try {
    $env:TMP = $temporaryRoot
    $env:TEMP = $temporaryRoot
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:PATH = "$env:SystemRoot\System32"
    Remove-Item Env:TATSU_ASSETS,Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    $inputPath = Join-Path $root 'fixtures/D01.pdf'
    $report = Join-Path $output 'launch.json'
    $process = Start-Process -FilePath $exe -ArgumentList @('--measure',('"'+$inputPath+'"'),('"'+$report+'"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw 'Startup probe did not complete.' }
    if ((Test-Path -LiteralPath $owned) -or !(Test-Path -LiteralPath $unmarked) -or !(Test-Path -LiteralPath $wrongMarker) -or !(Test-Path -LiteralPath $junction) -or !(Test-Path -LiteralPath $nested)) {
        throw 'Startup removed unrelated or linked data, or failed to remove abandoned data.'
    }
    if ((Get-FileHash -LiteralPath (Join-Path $outside 'preserve.txt')).Hash -ne $expectedHash) { throw 'Linked target changed.' }
    [ordered]@{
        status='PASS';process_exit=$process.ExitCode;executable_sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLower();
        abandoned_removed=$true;unmarked_preserved=$true;different_marker_preserved=$true;
        directory_junction_preserved=$true;nested_junction_preserved=$true;linked_payload_unchanged=$true;
        scope='Actual packaged Windows app startup with isolated process TEMP/TMP and real NTFS junctions; not clean/offline acceptance'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'startup-cleanup.json') -Encoding utf8
} finally {
    $env:TMP = $previousTMP
    $env:TEMP = $previousTEMP
    $env:QT_QPA_PLATFORM = $previousPlatform
    $env:PATH = $previousPath
    $env:TATSU_ASSETS = $previousAssets
    $env:QT_PLUGIN_PATH = $previousPlugins
}
