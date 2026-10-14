param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$RuntimeDirectory='evidence/dev43-runtime-r1',
    [string]$AssetsRuntimeDirectory='dist/PDFTatsujin-0.5.0-dev36-working',
    [string]$Filter='M5RW',
    [ValidatePattern('^r[1-9][0-9]*$')][string]$RunTag='r1',
    [switch]$Full
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
function Local([string]$relative) {
    if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains('\') -or $relative -match '(^|/)\.\.(/|$)|:') {throw 'Unsafe project-relative path'}
    $absolute=[IO.Path]::GetFullPath((Join-Path $taskRoot $relative))
    if (!$absolute.StartsWith($taskRoot+'\',[StringComparison]::OrdinalIgnoreCase)) {throw 'Outside project'}
    return $absolute
}
function Sha([string]$path) {return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
function New-Json([string]$path,$value) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($value|ConvertTo-Json -Depth 25))
    $stream=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try {$stream.Write($bytes,0,$bytes.Length)} finally {$stream.Dispose()}
}
$output=Local $OutputDirectory;$runtime=Local $RuntimeDirectory;$assets=Local $AssetsRuntimeDirectory
$exe=Join-Path $runtime 'PDFTatsujin.exe'
$names=@('PDFTatsujin.exe','Pdf4QtLibCore.dll','Pdf4QtLibWidgets.dll')
$files=@($names|ForEach-Object {$path=Join-Path $runtime $_;$item=Get-Item -LiteralPath $path; @{path=$RuntimeDirectory+'/'+$_;bytes=$item.Length;sha256=(Sha $path)}})
if (Test-Path -LiteralPath $output) {throw 'Preserve earlier producer evidence; use new output directory'}
if (Get-Process -Name '*PDFTatsujin*',cmake,ninja,cl,link,pdftoppm,pdftotext,soffice,java -ErrorAction SilentlyContinue) {throw 'Parent serial build/test/engine gate failed'}
$stage=if($Full){'full'}else{'target'}
$temp=Local ('build/dev43-'+$stage+'-engine-temp-'+$RunTag)
if (Test-Path -LiteralPath $temp) {throw 'Preserve earlier TEMP; choose a new stage/run'}
New-Item -ItemType Directory -Path $output,$temp | Out-Null
$keys=@('PATH','TEMP','TMP','TATSU_ASSETS','QT_PLUGIN_PATH','QT_QPA_PLATFORM','TATSU_TEST_FILTER','TATSU_UI_REVIEW','TATSU_OFFICE_CONVERTER','TATSU_PDFA_JAVA','TATSU_PDFA_JAR','TATSU_DENIED_SAVE_DIR','TATSU_NATIVE_PDF_PRINTER')
$previous=@{};foreach($key in $keys){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$denied=$null;$originalAcl=$null;$restored=$true;$moduleRows=@();$process=$null;$success=$false
try {
    $env:PATH=$runtime+';'+$assets+';'+$env:SystemRoot+'\System32'
    $env:TEMP=$temp;$env:TMP=$temp;$env:TATSU_ASSETS=Join-Path $assets 'assets';$env:QT_PLUGIN_PATH=$assets;$env:QT_QPA_PLATFORM='offscreen';$env:TATSU_UI_REVIEW='1'
    $env:TATSU_TEST_FILTER=if($Full){$null}else{$Filter}
    if($Full){
        $lock=Get-Content -LiteralPath (Join-Path $taskRoot 'pdfa-engine-lock.json') -Raw|ConvertFrom-Json
        $env:TATSU_OFFICE_CONVERTER=Join-Path $taskRoot 'tools/libreoffice/26.8.1/runtime/LibreOffice/program/soffice.com'
        $env:TATSU_PDFA_JAVA=Join-Path $taskRoot $lock.java.executable;$env:TATSU_PDFA_JAR=Join-Path $taskRoot $lock.verapdf.jar
        foreach($key in @('TATSU_OFFICE_CONVERTER','TATSU_PDFA_JAVA','TATSU_PDFA_JAR')){if(!(Test-Path -LiteralPath ([Environment]::GetEnvironmentVariable($key,'Process')) -PathType Leaf)){throw ('Missing fixed full engine '+$key)}}
        $denied=Local ('evidence/access-denial-dev43-'+[Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $denied | Out-Null
        Copy-Item -LiteralPath (Join-Path $taskRoot 'fixtures/D01.pdf') -Destination (Join-Path $denied 'existing.pdf')
        $originalAcl=Get-Acl -LiteralPath $denied;$changedAcl=Get-Acl -LiteralPath $denied
        $identity=[Security.Principal.WindowsIdentity]::GetCurrent().User
        $rights=[Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles
        $rule=[Security.AccessControl.FileSystemAccessRule]::new($identity,$rights,'ContainerInherit,ObjectInherit','None','Deny')
        $changedAcl.AddAccessRule($rule);Set-Acl -LiteralPath $denied -AclObject $changedAcl
        $env:TATSU_DENIED_SAVE_DIR=$denied;$env:TATSU_NATIVE_PDF_PRINTER='1';$restored=$false
    }
    $arguments=@('--selftest',('"'+(Join-Path $taskRoot 'fixtures')+'"'),('"'+$output+'"'))
    $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output 'native-stdout.log') -RedirectStandardError (Join-Path $output 'native-stderr.log')
    while(!$process.HasExited){
        if(!$moduleRows.Count){
            try {
                $actual=@($process.Modules | Where-Object {$names -contains $_.ModuleName})
                if($actual.Count -eq 3){
                    foreach($module in $actual){
                        $expected=Join-Path $runtime $module.ModuleName
                        if(!([IO.Path]::GetFullPath($module.FileName)).Equals($expected,[StringComparison]::OrdinalIgnoreCase)){throw ('Unexpected loaded module '+$module.FileName)}
                    }
                    $moduleRows=@($actual|ForEach-Object {@{name=$_.ModuleName;path=[IO.Path]::GetFullPath($_.FileName);sha256=(Sha $_.FileName);process_id=$process.Id;observed_at_utc=(Get-Date).ToUniversalTime().ToString('o')}})
                }
            } catch {
                if($_.Exception.Message.StartsWith('Unexpected loaded module')){throw}
            }
        }
        [void]$process.WaitForExit(100)
        $process.Refresh()
    }
    $process.WaitForExit();$process.Refresh()
    $report=Get-Content -LiteralPath (Join-Path $output 'selftest.json') -Raw|ConvertFrom-Json
    $tests=@($report.tests);$bad=@($tests|Where-Object {$_.status -ne 'PASS' -or ($Filter -and !$Full -and !$_.name.Contains($Filter))})
    $success=$process.ExitCode -eq 0 -and $report.failures -eq 0 -and $tests.Count -gt 0 -and !$bad.Count -and $moduleRows.Count -eq 3
    New-Json (Join-Path $output 'modules.json') @{format='dev43-live-module-proof-v1';run=$OutputDirectory;runtime_directory=$runtime;process_executable=$exe;process_id=$process.Id;completed=$success;loaded_modules=$moduleRows}
    New-Json (Join-Path $output 'process-result.json') @{exit_code=$process.ExitCode;exe_sha256=(Sha $exe);executable=$exe;runtime_directory=$runtime;assets_runtime_directory=$assets;development_runtime_reuse=$true;completed=$success;headless=$true;filter=$(if($Full){$null}else{$Filter});test_count=$tests.Count;report_valid=$success;report_error=$(if($success){$null}else{'Native tests or fresh loaded-module proof failed'});loaded_module_proof='modules.json';loaded_module_proof_sha256=(Sha (Join-Path $output 'modules.json'))}
    $binding='evidence/dev43-runtime-bindings-'+$stage+'-'+$RunTag+'.json'
    New-Json (Local $binding) @{format='dev43-current-runtime-bindings-v1';run=$OutputDirectory;executable=$RuntimeDirectory+'/PDFTatsujin.exe';executable_sha256=(Sha $exe);runtime_directory=$RuntimeDirectory;files=$files;process_sha256=(Sha (Join-Path $output 'process-result.json'));loaded_module_proof_path=$OutputDirectory+'/modules.json';loaded_module_proof_sha256=(Sha (Join-Path $output 'modules.json'));loaded_modules=$moduleRows}
    foreach($row in $files){if((Sha (Local $row.path)) -ne $row.sha256){throw 'Runtime changed during tests'}}
    Write-Output ("Native $stage tests: $($tests.Count), failures: $($report.failures), loaded fresh modules: $($moduleRows.Count), exit: $($process.ExitCode)")
    if(!$success){throw 'Native producer failed; all evidence preserved'}
} finally {
    if($denied -and $originalAcl){Set-Acl -LiteralPath $denied -AclObject $originalAcl;$restored=(Get-Acl -LiteralPath $denied).Sddl -eq $originalAcl.Sddl}
    New-Json (Join-Path $output 'environment-result.json') @{acl_restored=$restored;denied_directory=$denied;native_printer_requested=[bool]$Full;temp=$temp;temp_cleanup='Parent separate ownership inventory required';volume_full='unexecuted';offscreen=$true}
    foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}
    if(!$restored){throw 'Original test-folder ACL not restored'}
}
