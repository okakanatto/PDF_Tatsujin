param(
    [string]$Target = 'PDFTatsujin',
    [switch]$Upstream,
    [ValidatePattern('^[D-Z]$')][string]$Drive = 'T',
    [string]$VisualStudioPath,
    [switch]$WithoutSelfTests,
    [ValidateRange(0,1000)][int]$CompilerTestDelayMs = 0,
    [switch]$WithoutCompilerQueueFix
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = "${Drive}:/"
# Query the Unicode API: subst.exe prints Japanese paths using the OEM code page.
if (-not ('Tatsu.BuildPaths' -as [type])) {
    Add-Type @'
using System.Runtime.InteropServices;
using System.Text;
namespace Tatsu {
    public static class BuildPaths {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        public static extern uint QueryDosDevice(string name, StringBuilder target, int length);
    }
}
'@
}
$buffer = New-Object System.Text.StringBuilder 32768
$hasMapping = [Tatsu.BuildPaths]::QueryDosDevice("${Drive}:", $buffer, $buffer.Capacity)
if ($hasMapping) {
    $mappedPath = $buffer.ToString()
    if (!$mappedPath.StartsWith('\??\') -or $mappedPath.Substring(4).TrimEnd('\') -ine $root.TrimEnd('\')) {
        throw "${Drive}: maps to another directory. Choose a free drive with -Drive."
    }
} elseif (Test-Path "${Drive}:\") {
    throw "${Drive}: is already in use. Choose a free drive with -Drive."
} else {
    & subst.exe "${Drive}:" $root
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create ASCII build path.' }
}

if (!$VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'Install Visual Studio C++ Build Tools, or pass -VisualStudioPath.' }
$developerCommand = Join-Path $VisualStudioPath 'Common7/Tools/VsDevCmd.bat'
$environmentLines = & cmd.exe /d /s /c "`"$developerCommand`" -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

$qt = "${source}tools/Qt/6.9.3/msvc2022_64"
$cmake = "${source}tools/python/cmake/data/bin/cmake.exe"
$ninja = "${source}tools/python/bin/ninja.exe"
$env:PATH = "${source}tools/python/bin;${qt}/bin;" + $env:PATH
$env:VSLANG = '1033'
if (!(Test-Path $cmake) -or !(Test-Path $ninja)) { throw 'Missing pinned dependencies. See docs/DEVELOPMENT.md.' }
if ($Upstream -and $Target -eq 'PDFTatsujin') { $Target = 'Pdf4QtViewer' }
$app = if ($Upstream) { 'OFF' } else { 'ON' }
$selftest = if ($WithoutSelfTests) { 'OFF' } else { 'ON' }
$queueFix = if ($WithoutCompilerQueueFix) { 'OFF' } else { 'ON' }
$oldLocation = Get-Location
try {
    Set-Location $source
    $build = "${source}build/app"
    & $cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DTATSU_BUILD_APP=$app" "-DTATSU_ENABLE_SELFTEST=$selftest" "-DTATSU_FIX_COMPILER_STARTUP=$queueFix" "-DTATSU_COMPILER_TEST_DELAY_MS=$CompilerTestDelayMs" '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$qt" "-DCMAKE_TOOLCHAIN_FILE=${source}tools/vcpkg/scripts/buildsystems/vcpkg.cmake" '-DVCPKG_MANIFEST_MODE=OFF'
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $LASTEXITCODE" }
    & $cmake --build $build --target $Target --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
} finally {
    Set-Location $oldLocation
}
