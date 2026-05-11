# Build leviathan-clipboard-helper.exe using clang-cl + Ninja under the
# Visual Studio Build Tools developer environment (provides MSVC headers/libs
# and Windows SDK link paths).
#
# Usage:
#   pwsh -NoProfile -ExecutionPolicy Bypass -File build.ps1 [-Config Release|Debug] [-Clean]

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir  = Join-Path $scriptDir 'build'

# Locate the latest Visual Studio installation (Build Tools, Professional, etc.).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere — install Visual Studio Build Tools first."
}

$vsInstall = & $vswhere -latest -products * -property installationPath
if (-not $vsInstall) {
    throw "No Visual Studio installation found via vswhere."
}
# Verify the C++ toolchain is actually present (vswhere component IDs vary across
# VS major versions; checking for cl.exe is more robust than -requires).
$msvcRoot = Join-Path $vsInstall 'VC\Tools\MSVC'
if (-not (Test-Path $msvcRoot)) {
    throw "MSVC C++ tools are not installed under $vsInstall — install workload Microsoft.VisualStudio.Workload.VCTools."
}

# Compose the MSVC build environment manually rather than calling VsDevCmd.bat.
# VsDevCmd silently fails on systems where its directory is not on PATH (e.g.
# when launched from a bash subshell): it depends on `vswhere` being callable
# without a full path, and falls back to a half-initialized state that only
# sets Windows SDK paths and omits the MSVC C/C++ toolchain libs (msvcrtd.lib,
# oldnames.lib, etc.). We sidestep that by reading the install location from
# vswhere directly and pointing INCLUDE/LIB/PATH at the MSVC + SDK trees.

# Locate the newest *complete* MSVC toolchain. Some installations leave behind
# half-extracted version directories (e.g. only Auxiliary\) when an upgrade was
# interrupted; we filter those out by requiring both `include\` and `lib\x64\`.
$msvcToolset = Get-ChildItem -Path $msvcRoot -Directory |
    Where-Object {
        (Test-Path (Join-Path $_.FullName 'include')) -and
        (Test-Path (Join-Path $_.FullName 'lib\x64')) -and
        ($_.Name -as [version])
    } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $msvcToolset) {
    throw "No complete MSVC toolchain (with include\\ and lib\\x64\\) found under $msvcRoot"
}
$msvcVersion = Split-Path -Leaf $msvcToolset

# Locate the newest Windows SDK installed under the standard Kits root.
$kitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not (Test-Path $kitsRoot)) {
    throw "Windows Kits 10 not installed at $kitsRoot"
}
$sdkVersion = Get-ChildItem -Path (Join-Path $kitsRoot 'Lib') -Directory |
    Where-Object { ($_.Name -match '^10\.') -and ($_.Name -as [version]) } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1 -ExpandProperty Name
if (-not $sdkVersion) {
    throw "No Windows 10 SDK version found under $kitsRoot\Lib"
}

Write-Host "==> Using MSVC $msvcVersion + Windows SDK $sdkVersion"

$includePaths = @(
    (Join-Path $msvcToolset 'include'),
    (Join-Path $kitsRoot   "Include\$sdkVersion\ucrt"),
    (Join-Path $kitsRoot   "Include\$sdkVersion\um"),
    (Join-Path $kitsRoot   "Include\$sdkVersion\shared"),
    (Join-Path $kitsRoot   "Include\$sdkVersion\winrt")
)
$libPaths = @(
    (Join-Path $msvcToolset 'lib\x64'),
    (Join-Path $kitsRoot    "Lib\$sdkVersion\ucrt\x64"),
    (Join-Path $kitsRoot    "Lib\$sdkVersion\um\x64")
)
$msvcBin = Join-Path $msvcToolset 'bin\HostX64\x64'
$sdkBin  = Join-Path $kitsRoot   "bin\$sdkVersion\x64"

$env:INCLUDE = ($includePaths -join ';')
$env:LIB     = ($libPaths -join ';')
$env:PATH    = "$msvcBin;$sdkBin;$env:PATH"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Write-Host "==> Configuring leviathan-clipboard-helper ($Config) at $buildDir"
# Ninja Multi-Config is the only single-pass generator that survives CMake 4.3's
# `unscanned_$Config` rule emission with clang-cl. The plain "Ninja" generator
# leaves `$Config` literally in rule names and ninja rejects the build graph.
#
# We do NOT use vcpkg / libprotobuf. The helper hand-rolls a minimal protobuf
# wire codec in src/proto_wire.* — see CMakeLists.txt for the rationale.
& cmake -S $scriptDir -B $buildDir `
    -G 'Ninja Multi-Config' `
    -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_CONFIGURATION_TYPES="Debug;Release" `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

Write-Host "==> Building leviathan-clipboard-helper.exe ($Config)"
& cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

# With Ninja Multi-Config, artifacts land in $buildDir\<Config>\.
$bin = Join-Path $buildDir "$Config\leviathan-clipboard-helper.exe"
if (-not (Test-Path $bin)) {
    throw "Build succeeded but $bin is missing"
}

Write-Host ""
Write-Host "Built: $bin"
Get-Item $bin | Select-Object Length, LastWriteTime | Format-List
