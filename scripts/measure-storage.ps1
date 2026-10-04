$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$sum=(Get-ChildItem -LiteralPath $root -Recurse -Force -File | Measure-Object Length -Sum).Sum
$result=[pscustomobject]@{MeasuredAt=(Get-Date -Format o);Bytes=$sum;GB=[math]::Round($sum/1e9,3);LimitBytes=20000000000}
$result | ConvertTo-Json
if($sum -ge 20000000000){throw '20GB limit reached; stop before further downloads/builds'}
