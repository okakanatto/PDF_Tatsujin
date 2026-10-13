$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security.Cryptography.Pkcs
$root=Split-Path $PSScriptRoot -Parent
$bytes=[IO.File]::ReadAllBytes((Join-Path $root 'fixtures/D08-signed.pdf'))
$ascii=[Text.Encoding]::ASCII.GetString($bytes)
$range=[regex]::Match($ascii,'/ByteRange\s*\[\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*\]')
if(!$range.Success){throw 'ByteRange not found'}
$n=@(1..4 | ForEach-Object {[int]$range.Groups[$_].Value})
$signed=New-Object byte[] ($n[1]+$n[3])
[Array]::Copy($bytes,$n[0],$signed,0,$n[1])
[Array]::Copy($bytes,$n[2],$signed,$n[1],$n[3])
$hex=[regex]::Match($ascii,'/Contents\s*<([0-9a-fA-F]+)>').Groups[1].Value
$cms=[Security.Cryptography.Pkcs.SignedCms]::new([Security.Cryptography.Pkcs.ContentInfo]::new($signed),$true)
$cms.Decode([Convert]::FromHexString($hex))
$cms.CheckSignature($true)
[pscustomobject]@{CryptographicSignature='PASS';CertificateTrust='Self-signed disposable test certificate; not trusted';ByteRange=$n;Subject=$cms.SignerInfos[0].Certificate.Subject} | ConvertTo-Json
