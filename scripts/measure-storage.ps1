param([switch]$Details)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$files=@(Get-ChildItem -LiteralPath $root -Recurse -Force -File)
$sum=($files | Measure-Object Length -Sum).Sum
$result=[pscustomobject]@{MeasuredAt=(Get-Date -Format o);Bytes=$sum;GB=[math]::Round($sum/1e9,3);LimitBytes=20000000000}
if($Details){
    $folders=[ordered]@{}
    foreach($folder in Get-ChildItem -LiteralPath $root -Force -Directory){$folders[$folder.Name]=0L}
    $rootBytes=0L
    foreach($file in $files){
        $relative=$file.FullName.Substring($root.Length+1)
        $separator=$relative.IndexOf('\')
        if($separator -lt 0){$rootBytes+=$file.Length}
        else{$folders[$relative.Substring(0,$separator)]+=$file.Length}
    }
    $result | Add-Member -NotePropertyName Folders -NotePropertyValue $folders
    $result | Add-Member -NotePropertyName RootFilesBytes -NotePropertyValue $rootBytes
}
$result | ConvertTo-Json -Depth 4
if($sum -ge 20000000000){throw '20GB limit reached; stop before further downloads/builds'}
