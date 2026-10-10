param(
    [Parameter(Mandatory=$true)][string]$File,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$ExpectedFingerprint
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Preserve prior verification records.'}
$info=Get-Item -LiteralPath $File
if($info.Length -le 0 -or $info.Length -gt 256MB){throw 'Signed PDF size outside profile.'}
Add-Type -AssemblyName System.Security.Cryptography.Pkcs
$bytes=[IO.File]::ReadAllBytes($info.FullName)
$text=[Text.Encoding]::Latin1.GetString($bytes)
$matches=[regex]::Matches($text,'/ByteRange\s*\[\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*\]\s*/Contents\s*<([0-9a-fA-F]+)>')
if($matches.Count -ne 1){throw 'Expect one newly created signature for this inspection.'}
$range=@(1..4|ForEach-Object {[long]$matches[0].Groups[$_].Value})
if($range[0] -ne 0 -or $range[1] -le 0 -or $range[2] -le $range[1] -or $range[2] -ge $bytes.Length -or $range[3] -ne $bytes.Length-$range[2]){throw 'Signature does not cover current file.'}
if($range[1] -ne $matches[0].Groups[5].Index-1 -or $range[2] -ne $matches[0].Groups[5].Index+$matches[0].Groups[5].Length+1){throw 'Signature gap must contain exactly the Contents hex string.'}
$data=New-Object byte[] ($range[1]+$range[3])
[Array]::Copy($bytes,0,$data,0,$range[1])
[Array]::Copy($bytes,$range[2],$data,$range[1],$range[3])
$cms=[Security.Cryptography.Pkcs.SignedCms]::new([Security.Cryptography.Pkcs.ContentInfo]::new($data),$true)
$cms.Decode([Convert]::FromHexString($matches[0].Groups[5].Value))
$cms.CheckSignature($true)
if($cms.SignerInfos.Count -ne 1){throw 'Unexpected signer count.'}
$signer=$cms.SignerInfos[0]
$fingerprint=$signer.Certificate.GetCertHashString([Security.Cryptography.HashAlgorithmName]::SHA256).ToLower()
if($ExpectedFingerprint -and $fingerprint -ne $ExpectedFingerprint.ToLower()){throw 'Certificate differs from selected identity.'}
if($signer.DigestAlgorithm.Value -ne '2.16.840.1.101.3.4.2.1'){throw 'Expected explicit SHA-256 digest.'}
[ordered]@{status='PASS';engine='.NET SignedCms';file=$info.Name;file_sha256=(Get-FileHash -LiteralPath $info.FullName -Algorithm SHA256).Hash.ToLower();current_file_covered=$true;cryptographic_match=$true;digest='SHA-256';subject=$signer.Certificate.Subject;certificate_sha256=$fingerprint;chain_and_revocation_evaluated=$false;OS_trust_store_changed=$false} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Output "PASS: $($info.Name) mathematical signature and selected certificate"
