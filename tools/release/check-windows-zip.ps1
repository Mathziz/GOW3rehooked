# SPDX-License-Identifier: GPL-2.0-or-later
# Checks a Windows release ZIP before it is published:
# - the updaters of v0.1.5 and v0.1.6 accept it (the rules of their gow3_update.exe, src/main/update.cs until v0.1.6);
# - every program in it names itself in a version resource;
# - gow3_update.exe is the native stand-in, without .NET or networking, so no downloader ships again.
param(
    [Parameter(Mandatory)][string]$Zip,
    [switch]$Release # tag builds, whose ZIP must carry the stand-in for those updaters
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$programs = 'gow3.exe', 'gow3_debug.exe', 'gow3.dll', 'gow3_update.exe'
$cache = '_rehooked_/shader_cache/recipes-hash2-aes.bin'

# DLL names in a PE import table, and whether the file is a .NET assembly.
function Read-Imports([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    $sections = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $optional = $pe + 24
    $directories = $optional + $(if ([BitConverter]::ToUInt16($bytes, $optional) -eq 0x20b) { 112 } else { 96 })
    $table = $optional + [BitConverter]::ToUInt16($bytes, $pe + 20)
    $offset = {
        param([uint32]$Rva)
        for ($i = 0; $i -lt $sections; $i++) {
            $section = $table + 40 * $i
            $start = [BitConverter]::ToUInt32($bytes, $section + 12)
            $size = [Math]::Max([BitConverter]::ToUInt32($bytes, $section + 8), [BitConverter]::ToUInt32($bytes, $section + 16))
            if ($Rva -ge $start -and $Rva -lt $start + $size) { return $Rva - $start + [BitConverter]::ToUInt32($bytes, $section + 20) }
        }
        throw "$Path has an import outside its sections."
    }
    $dlls = @()
    $imports = [BitConverter]::ToUInt32($bytes, $directories + 8)
    if ($imports) {
        for ($descriptor = & $offset $imports; ($name = [BitConverter]::ToUInt32($bytes, $descriptor + 12)) -ne 0; $descriptor += 20) {
            $start = & $offset $name
            $end = [Array]::IndexOf($bytes, [byte]0, $start)
            $dlls += [Text.Encoding]::ASCII.GetString($bytes, $start, $end - $start)
        }
    }
    [pscustomobject]@{ Dlls = $dlls; DotNet = [BitConverter]::ToUInt32($bytes, $directories + 8 * 14 + 4) -ne 0 }
}

$size = (Get-Item -LiteralPath $Zip).Length
if ($size -le 0 -or $size -gt 128MB) { throw "$Zip is $size bytes: the updaters take up to 128 MiB." }
$work = Join-Path ([IO.Path]::GetTempPath()) "gow3-zip-check-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
    try {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $total = 0L
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ($name.EndsWith('/')) { continue }
            if ($name -cnotin $programs -and $name -cne $cache -and $name -cnotmatch '^licenses/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.txt$') {
                throw "The updaters refuse $name."
            }
            if (-not $names.Add($name) -or $entry.Length -le 0) { throw "$name is empty or appears twice." }
            $total += $entry.Length
            if ($name -cin $programs) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $work $name)) }
        }
        if ($total -gt 256MB) { throw "The files add up to $total bytes: the updaters take up to 256 MiB." }
        foreach ($program in $(if ($Release) { $programs } else { $programs[0..2] })) {
            if (-not $names.Contains($program)) { throw "The ZIP has no $program." }
        }
    } finally {
        $archive.Dispose()
    }

    foreach ($file in Get-ChildItem -LiteralPath $work -File) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($file.FullName)
        if ([string]::IsNullOrWhiteSpace($info.FileDescription) -or [string]::IsNullOrWhiteSpace($info.ProductName) -or
            [string]::IsNullOrWhiteSpace($info.FileVersion) -or $info.OriginalFilename -ne $file.Name) {
            throw "$($file.Name) has no complete version resource."
        }
    }
    $standIn = Join-Path $work 'gow3_update.exe'
    if (Test-Path -LiteralPath $standIn) {
        $imports = Read-Imports $standIn
        $others = @($imports.Dlls | Where-Object { $_ -notmatch '^kernel32\.dll$' })
        if ($imports.DotNet -or $others.Count -or (Get-Item -LiteralPath $standIn).Length -gt 1MB) {
            throw "gow3_update.exe is not the native stand-in (imports: $($imports.Dlls -join ', '))."
        }
    }
    Write-Output "$Zip passes the release checks."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
