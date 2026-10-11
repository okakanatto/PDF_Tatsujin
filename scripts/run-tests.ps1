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
    $reportPath=Join-Path $OutputDirectory 'selftest.json'
    $reportValid=$false
    $testCount=0
    $reportError='Selftest report is missing; no test execution was confirmed.'
    if(Test-Path -LiteralPath $reportPath){
        try {
            $report=Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json -ErrorAction Stop
            $selectedTests=@($report.tests)
            $testCount=$selectedTests.Count
            $invalidTests=@($selectedTests | Where-Object {
                $_.status -ne 'PASS' -or [string]::IsNullOrWhiteSpace($_.name) -or
                ($env:TATSU_TEST_FILTER -and !([string]$_.name).Contains($env:TATSU_TEST_FILTER))
            })
            $reportValid=$testCount -gt 0 -and $report.failures -eq 0 -and $invalidTests.Count -eq 0
            $reportError=if($reportValid){$null}else{'Selftest report has no passing tests, failures, or tests outside the requested filter.'}
        } catch {
            $reportError='Cannot validate selftest report: '+$_.Exception.Message
        }
    }
    [ordered]@{exit_code=$process.ExitCode;exe_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower();completed=($process.ExitCode -eq 0 -and $reportValid);headless=[bool]$Headless;filter=$env:TATSU_TEST_FILTER;test_count=$testCount;report_valid=$reportValid;report_error=$reportError} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'process-result.json')
    if(Test-Path -LiteralPath $reportPath){Get-Content -LiteralPath $reportPath}
    if($process.ExitCode -ne 0){throw "Selftest failed with exit code $($process.ExitCode). Evidence: $OutputDirectory"}
    if(!$reportValid){throw "$reportError Evidence: $OutputDirectory"}
} finally {
    $env:PATH=$previousPath
    $env:TATSU_ASSETS=$previousAssets
    $env:QT_PLUGIN_PATH=$previousPlugins
    $env:QT_QPA_PLATFORM=$previousPlatform
}
