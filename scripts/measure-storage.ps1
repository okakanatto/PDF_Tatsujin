param([string]$Root=(Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference='Stop'
$sum=(Get-ChildItem -LiteralPath $Root -Recurse -Force -File -ErrorAction Stop | Measure-Object Length -Sum).Sum
[pscustomobject]@{MeasuredAt=(Get-Date -Format o);Bytes=$sum;GB=[math]::Round($sum/1e9,3)} | ConvertTo-Json
