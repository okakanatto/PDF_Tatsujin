param(
    [Parameter(Mandatory=$true)][string]$PythonExecutable,
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$output=[IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$app=[IO.Path]::GetFullPath((Join-Path $root $AppDirectory))
if(!$app.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or !(Split-Path $app -Leaf).EndsWith('-working')){
    throw 'Use an owned development working runtime; preserve final candidates.'
}
$sourceCommit=(& git -C $root rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0){throw 'Cannot record source identity'}
if(!$output.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)){
    throw 'Use a fresh owned output directory inside this checkout.'
}
if(!(Test-Path -LiteralPath (Join-Path $app 'Qt6Core.dll')) -or !(Test-Path -LiteralPath (Join-Path $app 'assets/fonts/NotoSansJP.ttf'))){
    throw 'Use an existing app-local development runtime with its assets.'
}
New-Item -ItemType Directory -Path $output | Out-Null
# A successful build is required before copying any diagnostic executable.
& (Join-Path $PSScriptRoot 'build.ps1') -Target PDFTatsujinRedactionProbe *> (Join-Path $output 'build.log')
$built=Join-Path $root 'build/app/bin/PDFTatsujinRedactionProbe.exe'
$expected=(Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash.ToLower()
$probe=Join-Path $app ('PDFTatsujinRedactionProbe-'+[guid]::NewGuid().ToString('N')+'.exe')
if(Test-Path -LiteralPath $probe){throw 'Unexpected diagnostic collision'}
Copy-Item -LiteralPath $built -Destination $probe
$oldPath=$env:PATH; $oldAssets=$env:TATSU_ASSETS; $oldPlugins=$env:QT_PLUGIN_PATH; $oldPlatform=$env:QT_QPA_PLATFORM
try {
    $fixture=Join-Path $output 'fixture'
    & $PythonExecutable (Join-Path $PSScriptRoot 'prepare-redaction-probe.py') --output $fixture *> (Join-Path $output 'fixture.log')
    if($LASTEXITCODE -ne 0){throw 'Synthetic fixture generation failed'}
    $env:PATH="$env:SystemRoot\System32"
    Remove-Item Env:TATSU_ASSETS -ErrorAction SilentlyContinue
    Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    $env:QT_QPA_PLATFORM='offscreen'
    $run=Join-Path $output 'run'
    $arguments=@(('"'+(Join-Path $fixture 'unsafe-source.pdf')+'"'),('"'+(Join-Path $fixture 'plan.json')+'"'),('"'+$run+'"'))
    $process=Start-Process -FilePath $probe -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    [ordered]@{exit_code=$process.ExitCode;probe_sha256=$expected;source_commit=$sourceCommit;scope='Synthetic experimental subset; no product security acceptance'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'process-result.json') -Encoding utf8
    if($process.ExitCode -ne 0){throw 'Diagnostic failed; preserve output and checkpoints'}
    & $PythonExecutable (Join-Path $PSScriptRoot 'evaluate-redaction-probe.py') --fixture $fixture --run $run *> (Join-Path $output 'independent.log')
    if($LASTEXITCODE -ne 0){throw 'Independent inspection failed; preserve all outputs'}
    $result=Get-Content -LiteralPath (Join-Path $run 'independent.json') -Raw | ConvertFrom-Json
    $candidate=@($result.strategies | Where-Object {$_.strategy -eq 'sanitized-trial'})
    if($candidate.Count -ne 1 -or !$candidate[0].accepted -or $result.candidate_checks.status -ne 'PASS'){
        throw 'Experimental candidate did not meet this fixture scope'
    }
    Write-Output 'Executed: experimental candidate, independent inspection, failure/cancel/reedit checks. General security and product UI acceptance are not declared.'
} finally {
    $env:PATH=$oldPath; $env:TATSU_ASSETS=$oldAssets; $env:QT_PLUGIN_PATH=$oldPlugins; $env:QT_QPA_PLATFORM=$oldPlatform
    # Only our unique executable is eligible; do not touch other runtime files.
    if((Test-Path -LiteralPath $probe) -and (Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash.ToLower() -eq $expected){
        Remove-Item -LiteralPath $probe
    }
}
