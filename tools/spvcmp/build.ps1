# Builds spvcmp.exe with the EWDK clang-cl toolchain (same environment as build-fast.ps1).
$ErrorActionPreference = 'Stop'
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB) -join ';'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vk = "$env:GOW3_SDK_DIR\VulkanSDK"
$sh = Join-Path $repo 'src\gpu\shaders'
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W3 /Z7 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DXXH_INLINE_ALL /DWIN32_LEAN_AND_MEAN -fuse-ld=lld `
    /clang:-march=x86-64-v3 /clang:-ffp-contract=off (Join-Path $PSScriptRoot 'spvcmp.cpp') (Join-Path $sh 'gcn_translator.cpp') (Join-Path $sh 'gcn_spirv.cpp') (Join-Path $sh 'shader_parser.cpp') (Join-Path $sh 'shader_catalog.cpp') (Join-Path $repo 'src\gpu\piece_hash.cpp') `
    "/I$(Join-Path $repo 'src')" "/I$vk\Include" "/I$(Join-Path $repo 'src\externals\xxhash')" `
    "/Fe$(Join-Path $PSScriptRoot 'spvcmp.exe')" "/Fo$PSScriptRoot\" /link /DEBUG "$vk\Lib\shaderc_combined.lib"
if ($LASTEXITCODE -ne 0) { Write-Error "BUILD FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
'BUILD SUCCEEDED: spvcmp.exe'
