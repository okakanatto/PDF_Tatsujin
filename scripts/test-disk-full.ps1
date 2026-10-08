param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('Check','Create','Execute')][string]$Mode = 'Check',
    [string]$AppDirectory = 'dist/PDFTatsujin-0.2.0-rc9-working',
    [string]$Harness = 'build/app/bin/PDFTatsujinFilesystemProbe.exe'
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
$taskOutput = [IO.Path]::GetFullPath((Join-Path $taskRoot $OutputDirectory))
if ((Split-Path $taskOutput -Parent) -ine (Join-Path $taskRoot 'evidence') -or (Test-Path -LiteralPath $taskOutput)) {
    throw 'Use a new direct child of this checkout evidence directory.'
}
New-Item -ItemType Directory -Path $taskOutput | Out-Null
$taskAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not ('Tatsu.CapacityNative' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
namespace Tatsu {
    [StructLayout(LayoutKind.Sequential)]
    public struct CapacityStorageType { public uint DeviceId; public Guid VendorId; }
    [StructLayout(LayoutKind.Explicit, Size=56)]
    public struct CapacityCreateParameters {
        [FieldOffset(0)] public uint Version;
        [FieldOffset(8)] public Guid UniqueId;
        [FieldOffset(24)] public ulong MaximumSize;
        [FieldOffset(32)] public uint BlockSize;
        [FieldOffset(36)] public uint SectorSize;
        [FieldOffset(40)] public IntPtr ParentPath;
        [FieldOffset(48)] public IntPtr SourcePath;
    }
    public static class CapacityNative {
        [DllImport("virtdisk.dll", CharSet=CharSet.Unicode)]
        public static extern uint CreateVirtualDisk(ref CapacityStorageType type, string path,
            uint access, IntPtr security, uint flags, uint providerFlags,
            ref CapacityCreateParameters parameters, IntPtr overlapped, out IntPtr handle);
        [DllImport("kernel32.dll")]
        public static extern bool CloseHandle(IntPtr handle);
    }
}
'@
}
if ([IntPtr]::Size -ne 8 -or
    [Runtime.InteropServices.Marshal]::SizeOf([type][Tatsu.CapacityCreateParameters]) -ne 56 -or
    [Runtime.InteropServices.Marshal]::SizeOf([type][Tatsu.CapacityStorageType]) -ne 20) {
    throw 'Unexpected x64 virtual-disk ABI.'
}
$taskRecord = [ordered]@{
    mode=$Mode; elevated_admin=$taskAdmin; maximum_virtual_bytes=67108864;
    volumes_changed=$false; status='未実行';
    constraint='Attaching and formatting the newly created VHD requires an elevated administrator process.';
    script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLower()
}
foreach ($taskCommand in 'Mount-DiskImage','Get-DiskImage','Get-Disk','Initialize-Disk','New-Partition','Format-Volume','Add-PartitionAccessPath','Remove-PartitionAccessPath','Dismount-DiskImage') {
    Get-Command $taskCommand -ErrorAction Stop | Out-Null
}
if ($Mode -eq 'Check' -or ($Mode -eq 'Execute' -and !$taskAdmin)) {
    $taskRecord.status = if ($taskAdmin) { 'READY_TO_EXECUTE' } else { 'ENVIRONMENT_CONSTRAINT' }
    if ($taskAdmin) { $taskRecord.constraint = '' }
    $taskRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOutput 'environment.json') -Encoding utf8
    if ($Mode -eq 'Execute') { throw 'Run Execute in an elevated administrator console; no VHD was created or mounted.' }
    $taskRecord | ConvertTo-Json -Depth 5
    return
}
$taskImage = Join-Path $taskOutput 'owned-64MiB.vhd'
$taskMount = Join-Path $taskOutput 'volume'
$taskStorage = New-Object Tatsu.CapacityStorageType
$taskStorage.DeviceId = 2
$taskStorage.VendorId = [Guid]'EC984AEC-A0F9-47E9-901F-71415A66345B'
$taskParameters = New-Object Tatsu.CapacityCreateParameters
$taskParameters.Version = 1
$taskParameters.UniqueId = [Guid]::NewGuid()
$taskParameters.MaximumSize = 67108864
$taskParameters.SectorSize = 512
$taskHandle = [IntPtr]::Zero
$taskCreateCode = [Tatsu.CapacityNative]::CreateVirtualDisk([ref]$taskStorage,$taskImage,0x100000,[IntPtr]::Zero,0,0,[ref]$taskParameters,[IntPtr]::Zero,[ref]$taskHandle)
if ($taskCreateCode -ne 0) {
    $taskRecord.status = if ($taskCreateCode -in @(5,1314)) { 'ENVIRONMENT_CONSTRAINT' } else { 'FAIL_CREATE' }
    $taskRecord.create_error_code = $taskCreateCode
    $taskRecord.constraint = 'CreateVirtualDisk failed before any disk attachment or volume change.'
    $taskRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOutput 'environment.json') -Encoding utf8
    throw "CreateVirtualDisk failed: $taskCreateCode"
}
try { $taskRecord.create_status = 'PASS'; $taskRecord.virtual_id = $taskParameters.UniqueId.ToString() }
finally { [Tatsu.CapacityNative]::CloseHandle($taskHandle) | Out-Null }
if ($Mode -eq 'Create') {
    $taskRecord.status = 'PREPARED_ONLY'
    $taskRecord.image_sha256 = (Get-FileHash -LiteralPath $taskImage -Algorithm SHA256).Hash.ToLower()
    $taskRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOutput 'environment.json') -Encoding utf8
    $taskRecord | ConvertTo-Json -Depth 5
    return
}
$taskProduct = [IO.Path]::GetFullPath((Join-Path $taskRoot $AppDirectory))
$taskHarness = [IO.Path]::GetFullPath((Join-Path $taskRoot $Harness))
if (!$taskProduct.StartsWith($taskRoot + '\',[StringComparison]::OrdinalIgnoreCase) -or
    !$taskHarness.StartsWith($taskRoot + '\',[StringComparison]::OrdinalIgnoreCase) -or
    !(Test-Path -LiteralPath $taskHarness -PathType Leaf) -or
    !(Test-Path -LiteralPath (Join-Path $taskProduct 'PDFTatsujin.exe') -PathType Leaf) -or
    !(Test-Path -LiteralPath (Join-Path $taskProduct 'assets') -PathType Container)) {
    $taskRecord.status = 'FAIL_PREFLIGHT'
    $taskRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOutput 'environment.json') -Encoding utf8
    throw 'Prepare product assets and harness inside this checkout before attaching the test image.'
}
$taskMounted = $false
$taskAccessAdded = $false
$taskPartition = $null
$taskNumber = $null
$taskExistingNumbers = @(Get-Disk | ForEach-Object { $_.Number })
$taskCleanupErrors = @()
function Get-OwnedCapacityDisk {
    $taskFound = @((Get-DiskImage -ImagePath $taskImage) | Get-Disk)
    if ($taskFound.Count -ne 1 -or $taskFound[0].Size -ne 67108864 -or
        $taskFound[0].CimInstanceProperties['BusType'].Value -ne 15 -or $taskFound[0].IsBoot -or
        $taskFound[0].IsSystem -or $taskFound[0].Number -in $taskExistingNumbers -or
        ($null -ne $taskNumber -and $taskFound[0].Number -ne $taskNumber)) {
        throw 'Refusing a disk which is not the newly attached owned 64 MiB VHD.'
    }
    return $taskFound[0]
}
try {
    Mount-DiskImage -ImagePath $taskImage -Access ReadWrite -NoDriveLetter | Out-Null
    $taskMounted = $true
    $taskDisk = Get-OwnedCapacityDisk
    $taskNumber = $taskDisk.Number
    if ($taskDisk.PartitionStyle -ne 'RAW') { throw 'New test VHD is not raw.' }
    # These operations use the object resolved from the newly created image,
    # never a user-supplied physical disk number or drive letter.
    Initialize-Disk -InputObject (Get-OwnedCapacityDisk) -PartitionStyle MBR | Out-Null
    $taskPartition = New-Partition -DiskNumber (Get-OwnedCapacityDisk).Number -UseMaximumSize
    if ($taskPartition.DiskNumber -ne (Get-OwnedCapacityDisk).Number) { throw 'Partition ownership mismatch.' }
    Format-Volume -Partition $taskPartition -FileSystem NTFS -NewFileSystemLabel 'TATSU-TEST' -Confirm:$false -Force | Out-Null
    New-Item -ItemType Directory -Path $taskMount | Out-Null
    Add-PartitionAccessPath -InputObject $taskPartition -AccessPath ($taskMount + '\')
    $taskAccessAdded = $true
    [IO.File]::WriteAllText((Join-Path $taskMount '.tatsu-capacity-owner'),("PDFTatsujin capacity probe v1`n" + $taskParameters.UniqueId.ToString()),[Text.UTF8Encoding]::new($false))
    $taskRunner = Join-Path $taskOutput 'runner'
    New-Item -ItemType Directory -Path $taskRunner | Out-Null
    $taskExe = Join-Path $taskRunner 'PDFTatsujinFilesystemProbe.exe'
    Copy-Item -LiteralPath $taskHarness -Destination $taskExe
    $taskRecord.harness_sha256 = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLower()
    $taskRecord.product_exe_sha256 = (Get-FileHash -LiteralPath (Join-Path $taskProduct 'PDFTatsujin.exe') -Algorithm SHA256).Hash.ToLower()
    $taskOldPath = $env:PATH
    $taskOldAssets = $env:TATSU_ASSETS
    $taskOldPlugins = $env:QT_PLUGIN_PATH
    $taskOldPlatform = $env:QT_QPA_PLATFORM
    try {
        $env:PATH = "$taskProduct;$env:SystemRoot\System32"
        $env:TATSU_ASSETS = Join-Path $taskProduct 'assets'
        $env:QT_PLUGIN_PATH = $taskProduct
        $env:QT_QPA_PLATFORM = 'offscreen'
        $taskRun = Join-Path $taskOutput 'run'
        $taskArguments = @('--disk-full',('"'+(Join-Path $taskRoot 'fixtures')+'"'),('"'+$taskRun+'"'),('"'+$taskMount+'"'))
        $taskProcess = Start-Process -FilePath $taskExe -ArgumentList $taskArguments -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $taskOutput 'stdout.log') -RedirectStandardError (Join-Path $taskOutput 'stderr.log')
        if ($taskProcess.ExitCode -ne 0) { throw "Disk-full probe failed: $($taskProcess.ExitCode)" }
        $taskProbe = Get-Content -Raw -LiteralPath (Join-Path $taskRun 'filesystem-probe.json') | ConvertFrom-Json
        if ($taskProbe.tests.Count -ne 1 -or $taskProbe.tests[0].status -ne 'PASS') { throw 'Disk-full probe incomplete.' }
        $taskRecord.status = 'PASS'
        $taskRecord.probe = $taskProbe
    } finally {
        $env:PATH = $taskOldPath
        $env:TATSU_ASSETS = $taskOldAssets
        $env:QT_PLUGIN_PATH = $taskOldPlugins
        $env:QT_QPA_PLATFORM = $taskOldPlatform
    }
} catch {
    $taskRecord.status = 'FAIL'
    $taskRecord.error = $_.Exception.Message
    throw
} finally {
    # A failed mount-path removal must not prevent detaching our own image.
    if ($taskAccessAdded) {
        try { Remove-PartitionAccessPath -InputObject $taskPartition -AccessPath ($taskMount + '\') }
        catch { $taskCleanupErrors += $_.Exception.Message }
    }
    if ($taskMounted) {
        try {
            Dismount-DiskImage -ImagePath $taskImage
            $taskRecord.detached = !(Get-DiskImage -ImagePath $taskImage).Attached
            if (!$taskRecord.detached) { throw 'Owned capacity image is still attached.' }
        } catch { $taskCleanupErrors += $_.Exception.Message }
    }
    if ($taskCleanupErrors.Count) {
        $taskRecord.status = 'FAIL'
        $taskRecord.cleanup_errors = $taskCleanupErrors
    }
    $taskRecord.volumes_changed = $taskMounted
    $taskRecord | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $taskOutput 'environment.json') -Encoding utf8
}
if ($taskCleanupErrors.Count) { throw 'Capacity test cleanup failed; see environment.json.' }
$taskRecord | ConvertTo-Json -Depth 12
