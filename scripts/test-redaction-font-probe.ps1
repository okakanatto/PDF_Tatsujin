param(
    [Parameter(Mandatory=$true)][string]$PythonExecutable,
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [Parameter(Mandatory=$true)][string]$FixtureDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$app=[IO.Path]::GetFullPath((Join-Path $projectRoot $AppDirectory))
$fixture=[IO.Path]::GetFullPath((Join-Path $projectRoot $FixtureDirectory))
$output=[IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
foreach($path in @($app,$fixture,$output)){
    if(!$path.StartsWith($projectRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Use owned workspace paths'}
}
if(!(Split-Path $app -Leaf).EndsWith('-working') -or (Test-Path -LiteralPath $output)){throw 'Preserve final artifacts and previous results'}
$criteria=Get-Content -LiteralPath (Join-Path $fixture 'criteria.json') -Raw | ConvertFrom-Json
foreach($case in $criteria.cases){
    if([IO.Path]::GetFileName($case.file) -ne $case.file -or (Get-FileHash -LiteralPath (Join-Path $fixture $case.file)).Hash.ToLower() -ne $case.sha256){throw 'Changed fixture or unsafe filename'}
    $plan=if($case.plan){$case.plan}else{'plan.json'}
    if([IO.Path]::GetFileName($plan) -ne $plan -or !(Test-Path -LiteralPath (Join-Path $fixture $plan))){throw 'Missing plan or unsafe filename'}
}
New-Item -ItemType Directory -Path $output | Out-Null
& (Join-Path $PSScriptRoot 'build.ps1') -Target PDFTatsujinRedactionProbe *> (Join-Path $output 'build.log')
$built=Join-Path $projectRoot 'build/app/bin/PDFTatsujinRedactionProbe.exe'
$expected=(Get-FileHash -LiteralPath $built).Hash.ToLower()
$probe=Join-Path $app ('PDFTatsujinFontProbe-'+[guid]::NewGuid().ToString('N')+'.exe')
Copy-Item -LiteralPath $built -Destination $probe
$oldPath=$env:PATH; $oldPlatform=$env:QT_QPA_PLATFORM; $oldAssets=$env:TATSU_ASSETS; $oldPlugins=$env:QT_PLUGIN_PATH
try{
    $env:PATH="$env:SystemRoot\System32"; $env:QT_QPA_PLATFORM='offscreen'
    Remove-Item Env:TATSU_ASSETS,Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    foreach($case in $criteria.cases){
        $source=Join-Path $fixture $case.file
        $destination=Join-Path $output ([IO.Path]::GetFileNameWithoutExtension($case.file))
        $plan=if($case.plan){$case.plan}else{'plan.json'}
        $arguments=@('--candidate-only',('"'+$source+'"'),('"'+(Join-Path $fixture $plan)+'"'),('"'+$destination+'"'))
        $process=Start-Process -FilePath $probe -ArgumentList $arguments -WindowStyle Hidden -PassThru
        if(!$process.WaitForExit(60000)){$process.Kill();throw 'Owned diagnostic timed out'}
        [ordered]@{exit_code=$process.ExitCode;probe_sha256=$expected;source_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLower();scope='Synthetic internal candidate; no product acceptance'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'process-result.json') -Encoding utf8
        if((Get-FileHash -LiteralPath $source).Hash.ToLower() -ne $case.sha256){throw 'Source changed'}
        if($case.expect_rejection -and $process.ExitCode -ne 1){throw 'Unsafe profile unexpectedly succeeded'}
        if(!$case.expect_rejection -and $process.ExitCode -ne 0){throw 'Supported synthetic case failed'}
    }
    & $PythonExecutable (Join-Path $PSScriptRoot 'evaluate-redaction-font-probe.py') --fixture $fixture --run $output *> (Join-Path $output 'independent.log')
    if($LASTEXITCODE -ne 0){throw 'Independent font inspection failed'}
    Write-Output "PASS: $($criteria.cases.Count) font cases with independently inspected output or explicit rejection"
}finally{
    $env:PATH=$oldPath; $env:QT_QPA_PLATFORM=$oldPlatform; $env:TATSU_ASSETS=$oldAssets; $env:QT_PLUGIN_PATH=$oldPlugins
    if((Test-Path -LiteralPath $probe) -and (Get-FileHash -LiteralPath $probe).Hash.ToLower() -eq $expected){Remove-Item -LiteralPath $probe}
}
