# Builds etlprof.exe with the EWDK clang-cl toolchain (same environment as build-fast.ps1).
$ErrorActionPreference = 'Stop'
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB) -join ';'
$src = Join-Path $PSScriptRoot 'etlprof.cpp'
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /std:c++20 /EHsc /utf-8 /W3 /Z7 -fuse-ld=lld $src `
    "/Fe$(Join-Path $PSScriptRoot 'etlprof.exe')" "/Fo$(Join-Path $PSScriptRoot 'etlprof.obj')" /link /DEBUG
if ($LASTEXITCODE -ne 0) { Write-Error "BUILD FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
'BUILD SUCCEEDED: etlprof.exe'
