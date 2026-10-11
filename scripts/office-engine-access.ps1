param(
    [Parameter(Mandatory)][string]$EngineRoot,
    [Parameter(Mandatory)][string]$Sid,
    [Parameter(Mandatory)][string]$Backup,
    [ValidateSet('Grant','Restore')][string]$Mode
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$engine=(Resolve-Path -LiteralPath $EngineRoot).Path
$allowed=[IO.Path]::GetFullPath((Join-Path $root 'tools/libreoffice/'))
if(!$engine.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or
    !(Test-Path -LiteralPath (Join-Path $engine 'program/soffice.com')) -or
    ((Get-Item -LiteralPath $engine).Attributes -band [IO.FileAttributes]::ReparsePoint)){
    throw 'Only the owned LibreOffice runtime is eligible for a temporary test identity'
}
$record=[IO.Path]::GetFullPath($Backup)
if(!$record.StartsWith([IO.Path]::GetFullPath((Join-Path $root 'evidence/')),[StringComparison]::OrdinalIgnoreCase)){
    throw 'ACL evidence must remain inside this workspace'
}
if($Mode -eq 'Grant'){
    if(Test-Path -LiteralPath $record){throw 'Preserve the earlier ACL record'}
    $acl=Get-Acl -LiteralPath $engine
    $original=$acl.Sddl
    [ordered]@{engine=$engine;sid=$Sid;sddl=$original} | ConvertTo-Json | Set-Content -LiteralPath $record
    $identity=[Security.Principal.SecurityIdentifier]::new($Sid)
    $rule=[Security.AccessControl.FileSystemAccessRule]::new($identity,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $engine -AclObject $acl
} else {
    $saved=Get-Content -LiteralPath $record -Raw | ConvertFrom-Json
    if($saved.engine -ine $engine -or $saved.sid -ne $Sid){throw 'ACL record identity mismatch'}
    $acl=Get-Acl -LiteralPath $engine
    $acl.SetSecurityDescriptorSddlForm($saved.sddl)
    Set-Acl -LiteralPath $engine -AclObject $acl
    if((Get-Acl -LiteralPath $engine).Sddl -ne $saved.sddl){throw 'Engine ACL restore mismatch'}
}
