param([string]$AppDirectory = 'dist/PDFTatsujin-M1-review')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$parent = Join-Path $root ('evidence/ntfs-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$denied = Join-Path $parent 'denied'
if (Test-Path -LiteralPath $parent) { throw 'Use a new test directory.' }
New-Item -ItemType Directory -Path $denied | Out-Null
Copy-Item (Join-Path $root 'fixtures/D01.pdf') (Join-Path $denied 'existing.pdf')
$original = Get-Acl -LiteralPath $denied
$original.Sddl | Set-Content (Join-Path $parent 'original-acl.txt')
$changed = Get-Acl -LiteralPath $denied
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
$rights = [Security.AccessControl.FileSystemRights]'Write,Delete,DeleteSubdirectoriesAndFiles'
$inheritance = [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit'
$rule = [Security.AccessControl.FileSystemAccessRule]::new(
    $identity, $rights, $inheritance,
    [Security.AccessControl.PropagationFlags]::None,
    [Security.AccessControl.AccessControlType]::Deny
)
$changed.AddAccessRule($rule)
$previousDirectory = $env:TATSU_DENIED_SAVE_DIR
$previousFilter = $env:TATSU_TEST_FILTER
try {
    Set-Acl -LiteralPath $denied -AclObject $changed
    $env:TATSU_DENIED_SAVE_DIR = $denied
    $env:TATSU_TEST_FILTER = 'A10_NTFS_access_denied'
    & (Join-Path $root 'scripts/run-tests.ps1') -Headless -AppDirectory $AppDirectory -OutputDirectory (Join-Path $parent 'results')
} finally {
    Set-Acl -LiteralPath $denied -AclObject $original
    $env:TATSU_DENIED_SAVE_DIR = $previousDirectory
    $env:TATSU_TEST_FILTER = $previousFilter
    if ((Get-Acl -LiteralPath $denied).Sddl -ne $original.Sddl) {
        throw "The test ACL was not restored. Original ACL: $parent/original-acl.txt"
    }
    Write-Output 'NTFS test ACL restored to the original SDDL.'
}
