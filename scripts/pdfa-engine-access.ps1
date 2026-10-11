param(
    [Parameter(Mandatory)][string]$EngineRoot,
    [Parameter(Mandatory)][string]$Sid,
    [Parameter(Mandatory)][string]$Backup,
    [ValidateSet('Grant','Restore')][string]$Mode
)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskEngine=(Resolve-Path -LiteralPath $EngineRoot).Path
$taskAllowed=[IO.Path]::GetFullPath((Join-Path $taskRoot 'tools/pdfa-verifier'))
if($taskEngine -ine $taskAllowed -or ((Get-Item -LiteralPath $taskEngine).Attributes -band [IO.FileAttributes]::ReparsePoint)){
    throw 'Only the owned PDF/A runtime is eligible for the temporary test identity'
}
$taskRecord=[IO.Path]::GetFullPath($Backup)
if(!$taskRecord.StartsWith([IO.Path]::GetFullPath((Join-Path $taskRoot 'evidence/')),[StringComparison]::OrdinalIgnoreCase)){
    throw 'ACL evidence must remain inside the workspace'
}
if($Mode -eq 'Grant'){
    if(@(Get-ChildItem -LiteralPath $taskEngine -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count){throw 'Runtime tree contains a reparse point'}
    if(Test-Path -LiteralPath $taskRecord){throw 'Preserve earlier ACL record'}
    $taskAcl=Get-Acl -LiteralPath $taskEngine
    [ordered]@{engine=$taskEngine;sid=$Sid;sddl=$taskAcl.Sddl}|ConvertTo-Json|Set-Content -LiteralPath $taskRecord
    $taskIdentity=[Security.Principal.SecurityIdentifier]::new($Sid)
    $taskRule=[Security.AccessControl.FileSystemAccessRule]::new($taskIdentity,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')
    $taskAcl.AddAccessRule($taskRule)
    Set-Acl -LiteralPath $taskEngine -AclObject $taskAcl
} else {
    $taskSaved=Get-Content -LiteralPath $taskRecord -Raw|ConvertFrom-Json
    if($taskSaved.engine -ine $taskEngine -or $taskSaved.sid -ne $Sid){throw 'ACL record identity mismatch'}
    $taskAcl=Get-Acl -LiteralPath $taskEngine
    $taskAcl.SetSecurityDescriptorSddlForm($taskSaved.sddl)
    Set-Acl -LiteralPath $taskEngine -AclObject $taskAcl
    if((Get-Acl -LiteralPath $taskEngine).Sddl -ne $taskSaved.sddl){throw 'PDF/A ACL restoration mismatch'}
}
