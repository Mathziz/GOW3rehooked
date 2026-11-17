foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB) -join ';'
Set-Location $PSScriptRoot
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /EHsc /std:c++20 /MT /D_CRT_SECURE_NO_WARNINGS -fuse-ld=lld $(if ($args[0]) { $args[0] } else { "lightcap.cpp" }) /Fe:$(if ($args[1]) { $args[1] } else { "lightcap.exe" })
"exit $LASTEXITCODE"
