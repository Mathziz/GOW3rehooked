# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateRange(1, 256)][int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Release,
    [switch]$ConfigureOnly,
    [switch]$NoDeploy
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$BuildType = if ($Release) { 'Release' } else { 'RelWithDebInfo' }
$BuildDirectory = Join-Path $Root "build-fast\$BuildType"
$GameDirectory = $env:GOW3_GAME_DIR  # the binaries are also copied there when set
$SdkDirectory = $env:GOW3_SDK_DIR    # EWDK, VulkanSDK, vcpkg and sccache-cache, when set
$Names = @('gow3.exe', 'gow3_debug.exe')

function Assert-Exit([string]$Action) {
    if ($LASTEXITCODE -ne 0) { throw "$Action failed with exit code $LASTEXITCODE." }
}

function Publish-Binaries([string]$Source, [string]$Destination) {
    $new = @{}
    foreach ($name in $Names) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $Source $name))
        if ($bytes.Length -lt 256 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) {
            throw "Invalid compiled binary: $name"
        }
        $pe = [BitConverter]::ToInt32($bytes, 0x3c)
        if ($pe -lt 0 -or $pe -gt $bytes.Length - 24 -or
            [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
            [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) {
            throw "Not an x64 PE binary: $name"
        }
        $new[$name] = $bytes
    }
    if (-not (Test-Path -LiteralPath $Destination -PathType Container)) {
        throw "Deployment directory does not exist: $Destination"
    }
    $opened = @()
    $writing = $false
    $committed = $false
    try {
        foreach ($name in $Names) {
            $path = Join-Path $Destination $name
            $existed = [IO.File]::Exists($path)
            $stream = [IO.File]::Open($path, [IO.FileMode]::OpenOrCreate,
                                     [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $item = @{ Path = $path; Stream = $stream; Existed = $existed; Original = $null; Name = $name }
            $opened += $item
            $backup = [IO.MemoryStream]::new()
            try { $stream.CopyTo($backup); $item.Original = $backup.ToArray() }
            finally { $backup.Dispose() }
        }
        # Hold every target exclusively before replacing any existing binary.
        $writing = $true
        foreach ($item in $opened) {
            $item.Stream.Position = 0
            $item.Stream.Write($new[$item.Name], 0, $new[$item.Name].Length)
            $item.Stream.SetLength($new[$item.Name].Length)
            $item.Stream.Flush($true)
        }
        $committed = $true
    } catch {
        $failure = $_
        $rollbackErrors = @()
        if ($writing) {
            foreach ($item in $opened) {
                try {
                    $item.Stream.Position = 0
                    $item.Stream.Write($item.Original, 0, $item.Original.Length)
                    $item.Stream.SetLength($item.Original.Length)
                    $item.Stream.Flush($true)
                } catch { $rollbackErrors += "$($item.Name): $($_.Exception.Message)" }
            }
        }
        if ($rollbackErrors.Count) {
            throw "Deployment failed: $failure. ROLLBACK FAILED: $($rollbackErrors -join '; ')"
        }
        throw $failure
    } finally {
        foreach ($item in $opened) { $item.Stream.Dispose() }
        if (-not $committed) {
            foreach ($item in $opened) {
                if (-not $item.Existed) { Remove-Item -LiteralPath $item.Path -Force }
            }
        }
    }
}

try {
    $environmentFile = "$SdkDirectory\EWDK\EWDK-LLVM.env"
    if ($SdkDirectory -and (Test-Path -LiteralPath $environmentFile)) {
        foreach ($line in Get-Content -LiteralPath $environmentFile) {
            if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
            $name, $value = $line -split '=', 2
            if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
        }
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        Assert-Exit 'Visual Studio discovery'
        $configuredVcpkg = $env:VCPKG_ROOT
        & "$visualStudio\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
        # Developer PowerShell replaces VCPKG_ROOT with Visual Studio's bundled instance.
        if ($configuredVcpkg) { $env:VCPKG_ROOT = $configuredVcpkg }
        $env:Version_Number = $env:WindowsSDKVersion.TrimEnd('\')
    }
    $includes = @('ucrt', 'shared', 'um', 'winrt', 'cppwinrt') |
        ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }
    $libraries = @('ucrt', 'um') |
        ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }
    $env:INCLUDE = (@($includes) + $env:INCLUDE) -join ';'
    $env:LIB = (@($libraries) + $env:LIB) -join ';'
    $env:VSCMD_SKIP_SENDTELEMETRY = '1'
    # The checkouts beside this one share the compiler cache.
    $env:SCCACHE_BASEDIRS = (Get-ChildItem -LiteralPath (Split-Path $Root) -Directory | ForEach-Object FullName) -join ';'
    if (-not $env:SCCACHE_DIR -and $SdkDirectory) { $env:SCCACHE_DIR = "$SdkDirectory\sccache-cache" }
    if (-not $env:SCCACHE_CACHE_SIZE) { $env:SCCACHE_CACHE_SIZE = '50G' }
    if (-not $env:VULKAN_SDK -and $SdkDirectory) { $env:VULKAN_SDK = "$SdkDirectory\VulkanSDK" }
    $vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "$SdkDirectory\vcpkg" }
    $shadercRoot = Join-Path $vcpkgRoot 'installed\x64-gow3-static'
    if (-not (Test-Path -LiteralPath (Join-Path $shadercRoot 'share\unofficial-shaderc\unofficial-shaderc-config.cmake'))) {
        $env:VCPKG_MAX_CONCURRENCY = "$Jobs"
        $env:PATH = (Join-Path $env:WindowsSdkDir "bin\$($env:Version_Number)\x64") + ';' + $env:PATH
        & (Join-Path $vcpkgRoot 'vcpkg.exe') install shaderc:x64-gow3-static --overlay-triplets "$Root\tools\vcpkg"
        Assert-Exit 'Static shaderc installation'
    }
    $compiler = Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe'
    $configure = @('-S', $Root, '-B', $BuildDirectory, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$BuildType", "-DCMAKE_C_COMPILER=$compiler", "-DCMAKE_CXX_COMPILER=$compiler",
        "-DCMAKE_LINKER=$(Join-Path $env:LLVM_ROOT 'bin\lld-link.exe')",
        "-DCMAKE_PREFIX_PATH=$shadercRoot",
        "-DCMAKE_AR=$(Join-Path $env:LLVM_ROOT 'bin\llvm-lib.exe')",
        "-DCMAKE_MT=$(Join-Path $env:LLVM_ROOT 'bin\llvm-mt.exe')",
        '-DCMAKE_C_COMPILER_LAUNCHER=sccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DCMAKE_CXX_FLAGS=/utf-8',
        # /Ob2: clang-cl /Ob1 marks every function not declared inline (templates, static helpers) noinline, even for LTO.
        '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob2 /DNDEBUG /Z7', '-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /Ob2 /DNDEBUG /Z7',
        '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG', '-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG')
    $glslc = Join-Path $shadercRoot 'tools\shaderc\glslc.exe'
    if (Test-Path -LiteralPath $glslc) { $configure += "-DVulkan_GLSLC_EXECUTABLE=$glslc" }
    & cmake @configure
    Assert-Exit 'Configuration'
    if ($ConfigureOnly) { exit 0 }
    & cmake --build $BuildDirectory --parallel $Jobs --target gow3 gow3_debug -- -k 0
    Assert-Exit 'Incremental build'
    $output = Join-Path $Root 'build'
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    Publish-Binaries (Join-Path $BuildDirectory 'bin') $output
    if (-not $NoDeploy -and $GameDirectory) { Publish-Binaries $output $GameDirectory }
    Write-Output "BUILD SUCCEEDED: $($Names -join ', ')"
    exit 0
} catch {
    Write-Error "BUILD FAILED: $($_.Exception.Message)" -ErrorAction Continue
    exit 1
}
