# Builds asmstat.exe with the EWDK clang-cl toolchain, linking the Zydis/Zycore libraries of build-fast.
$ErrorActionPreference = 'Stop'
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB) -join ';'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$zydis = Join-Path $root 'externals\zydis'
$lib = Join-Path $root 'build-fast\RelWithDebInfo\lib'
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /std:c++20 /EHsc /utf-8 /W3 /Z7 /MT -fuse-ld=lld -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD `
    "/I$(Join-Path $zydis 'include')" "/I$(Join-Path $zydis 'dependencies\zycore\include')" (Join-Path $PSScriptRoot 'asmstat.cpp') `
    "/Fe$(Join-Path $PSScriptRoot 'asmstat.exe')" "/Fo$(Join-Path $PSScriptRoot 'asmstat.obj')" `
    /link /DEBUG (Join-Path $lib 'Zydis.lib') (Join-Path $lib 'Zycore.lib') dbghelp.lib
if ($LASTEXITCODE -ne 0) { Write-Error "BUILD FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
'BUILD SUCCEEDED: asmstat.exe'
