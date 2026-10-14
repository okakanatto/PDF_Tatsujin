param(
    [string]$Fixtures = 'fixtures/certificate-signatures',
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Use a new output record; preserve previous results.'}
Add-Type -AssemblyName System.Security.Cryptography.Pkcs
$manifest = Get-Content -LiteralPath (Join-Path $Fixtures 'manifest.json') -Raw | ConvertFrom-Json
$records = @()
foreach($case in $manifest.cases){
    $path = Join-Path $Fixtures $case.file
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower() -ne $case.sha256){throw "Changed fixture: $($case.file)"}
    # Malformed range, blank and unsupported format cases are inspected by the PDF application.
    if($case.file -like 'range-*' -or $case.file -in @('blank.pdf','unknown.pdf','unbound.pdf')){continue}
    $data = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($path))
    $text = [Text.Encoding]::Latin1.GetString($data)
    $matches = [regex]::Matches($text, '/ByteRange\s*\[([^]]+)\]\s*/Contents\s*<([0-9a-fA-F]+)>')
    if($matches.Count -eq 0){throw "No actual signature found: $($case.file)"}
    foreach($match in $matches){
        $ranges = @($match.Groups[1].Value.Trim() -split '\s+' | ForEach-Object {[long]$_})
        if($ranges.Count -ne 4){throw 'Unexpected fixture range'}
        $signed = New-Object byte[] ($ranges[1] + $ranges[3])
        [Array]::Copy($data,0,$signed,0,$ranges[1])
        [Array]::Copy($data,$ranges[2],$signed,$ranges[1],$ranges[3])
        $der = [Convert]::FromHexString($match.Groups[2].Value)
        $actual = $false
        $errorText = $null
        try {
            $cms = [Security.Cryptography.Pkcs.SignedCms]::new([Security.Cryptography.Pkcs.ContentInfo]::new($signed),$true)
            $cms.Decode($der)
            # Check mathematical integrity without changing or trusting any OS certificate.
            $cms.CheckSignature($true)
            $actual = $true
        } catch { $errorText = $_.Exception.Message }
        if($case.file -eq 'weak-hash.pdf' -and !$actual -and $errorText.Contains('2.16.840.1.101.3.4.2.4')){
            $records += [ordered]@{file=$case.file;cryptographic_match=$null;status='未実行';reason='.NET SignedCms on this Windows does not support SHA-224; application rejection is tested separately.'}
            continue
        }
        $expected = $case.integrity -eq 'Unchanged' -or $case.file -like 'weak-*'
        if($actual -ne $expected){throw "Independent signature mismatch: $($case.file): $errorText"}
        $records += [ordered]@{file=$case.file;cryptographic_match=$actual;expected=$expected;entire_file=($ranges[2]+$ranges[3] -eq $data.Length);status='PASS';error=$errorText}
    }
}
if($records.Count -lt 10){throw 'Independent execution did not cover required cases.'}
[ordered]@{engine='.NET SignedCms';records=$records;OS_trust_store_changed=$false;revocation_checked=$false;scope='Independent mathematical signature verification; no trust or general acceptance claim'} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
$passed = @($records | Where-Object {$_.status -eq 'PASS'}).Count
Write-Output "Independent .NET verification: $passed PASS; $($records.Count - $passed) unsupported/未実行"
