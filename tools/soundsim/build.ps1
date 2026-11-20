# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object {
    Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_"
}) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object {
    Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64"
}) + $env:LIB) -join ';'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /MT /std:c++latest /EHsc /utf-8 /W4 /Z7 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS -fuse-ld=lld `
    "/I$repo\src" "/I$repo\src\externals\fmt\include" "/I$repo\src\externals\xxhash" `
    "$PSScriptRoot\soundsim.cpp" "$PSScriptRoot\capture.cpp" "/Fe$PSScriptRoot\soundsim.exe" "/Fo$PSScriptRoot\" /link /DEBUG
if ($LASTEXITCODE -ne 0) { throw "soundsim build failed ($LASTEXITCODE)" }
'BUILD SUCCEEDED: soundsim.exe'
