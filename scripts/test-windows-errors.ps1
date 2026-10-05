param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$AppDirectory = 'dist/PDFTatsujin-M1-reading',
    [switch]$IncludeNativePrinter
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$outputPath = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if (Test-Path -LiteralPath $outputPath) { throw 'Use a new evidence directory.' }
# Only change the ACL of a newly created folder inside this checkout.
$deniedPath = Join-Path $root ('evidence/access-denial-' + [Guid]::NewGuid().ToString('N'))
if (!$deniedPath.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Access-denial folder is outside the project.'
}
New-Item -ItemType Directory -Path $deniedPath | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'fixtures/D01.pdf') -Destination (Join-Path $deniedPath 'existing.pdf')
$originalAcl = Get-Acl -LiteralPath $deniedPath
$testAcl = Get-Acl -LiteralPath $deniedPath
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
$rights = [Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles
$inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit
$rule = New-Object Security.AccessControl.FileSystemAccessRule($identity, $rights, $inheritance, [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Deny)
$testAcl.AddAccessRule($rule)
$previousDenied = $env:TATSU_DENIED_SAVE_DIR
$previousPrinter = $env:TATSU_NATIVE_PDF_PRINTER
$restored = $false
try {
    Set-Acl -LiteralPath $deniedPath -AclObject $testAcl
    $env:TATSU_DENIED_SAVE_DIR = $deniedPath
    if ($IncludeNativePrinter) { $env:TATSU_NATIVE_PDF_PRINTER = '1' }
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Headless -AppDirectory $AppDirectory -OutputDirectory $outputPath
} finally {
    Set-Acl -LiteralPath $deniedPath -AclObject $originalAcl
    $restored = (Get-Acl -LiteralPath $deniedPath).Sddl -eq $originalAcl.Sddl
    $env:TATSU_DENIED_SAVE_DIR = $previousDenied
    $env:TATSU_NATIVE_PDF_PRINTER = $previousPrinter
    if (Test-Path -LiteralPath $outputPath) {
        @{ acl_restored=$restored; denied_directory=$deniedPath; native_printer_requested=[bool]$IncludeNativePrinter; executable_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path (Join-Path $root $AppDirectory) 'PDFTatsujin.exe')).Hash.ToLower(); volume_full='未実行; NTFS write denial is a separate test' } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputPath 'windows-errors-environment.json') -Encoding utf8
    }
    if (!$restored) { throw "Original ACL was not restored: $deniedPath" }
}
