# Builds varbench.exe with the EWDK clang-cl toolchain, same CPU baseline as the game (CMakeLists.txt).
$ErrorActionPreference = 'Stop'
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB) -join ';'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W3 /Z7 -fuse-ld=lld /clang:-march=x86-64-v3 /clang:-ffp-contract=off `
    (Join-Path $PSScriptRoot 'varbench.cpp') (Join-Path $repo 'src\gpu\shaders\gcn_translator.cpp') `
    "/I$(Join-Path $repo 'src')" "/I$(Join-Path $repo 'src\externals\xxhash')" "/I$env:GOW3_SDK_DIR\VulkanSDK\Include" `
    "/Fe$(Join-Path $PSScriptRoot 'varbench.exe')" "/Fo$PSScriptRoot\" /link /DEBUG "$env:GOW3_SDK_DIR\VulkanSDK\Lib\shaderc_combined.lib"
if ($LASTEXITCODE -ne 0) { Write-Error "BUILD FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
'BUILD SUCCEEDED: varbench.exe'
