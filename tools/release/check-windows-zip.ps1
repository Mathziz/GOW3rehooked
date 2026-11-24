# SPDX-License-Identifier: GPL-2.0-or-later
# Checks a Windows release ZIP before it is published:
# - the updater (src/main/update.cpp) accepts it;
# - gow3.exe names itself in a version resource and carries its real link time: a /Brepro hash in the PE timestamp turned
#   Microsoft Defender's machine learning against the executables of v0.1.5 to v0.2.1.
param([Parameter(Mandatory)][string]$Zip)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$files = 'gow3.exe', 'gow3_debug.cmd'
$cache = '_rehooked_/shader_cache/recipes-hash2-aes.bin'

$size = (Get-Item -LiteralPath $Zip).Length
if ($size -le 0 -or $size -gt 128MB) { throw "$Zip is $size bytes: the updater takes up to 128 MiB." }
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
            if ($name -cnotin $files -and $name -cne $cache -and $name -cnotmatch '^licenses/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.txt$') {
                throw "The updater refuses $name."
            }
            if (-not $names.Add($name) -or $entry.Length -le 0) { throw "$name is empty or appears twice." }
            $total += $entry.Length
            if ($name -ceq 'gow3.exe') { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $work $name)) }
        }
        if ($total -gt 256MB) { throw "The files add up to $total bytes: the updater takes up to 256 MiB." }
        foreach ($file in $files) { if (-not $names.Contains($file)) { throw "The ZIP has no $file." } }
    } finally {
        $archive.Dispose()
    }

    $exe = Join-Path $work 'gow3.exe'
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
    if ([string]::IsNullOrWhiteSpace($info.FileDescription) -or [string]::IsNullOrWhiteSpace($info.ProductName) -or
        [string]::IsNullOrWhiteSpace($info.FileVersion) -or $info.OriginalFilename -ne 'gow3.exe') {
        throw 'gow3.exe has no complete version resource.'
    }
    $bytes = [IO.File]::ReadAllBytes($exe)
    $linked = [DateTimeOffset]::FromUnixTimeSeconds([BitConverter]::ToUInt32($bytes, [BitConverter]::ToInt32($bytes, 0x3c) + 8))
    if ([Math]::Abs(($linked - [DateTimeOffset]::UtcNow).TotalDays) -gt 2) { throw "gow3.exe was not linked now ($linked): /Brepro?" }
    Write-Output "$Zip passes the release checks."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
