# Builds framecap.exe with the EWDK clang-cl toolchain (same environment as build-fast.ps1).
$ErrorActionPreference = 'Stop'
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB) -join ';'
$src = Join-Path $PSScriptRoot 'framecap.cpp'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
# xxhash is header-only here; zstd (the .rec records) is built once from zstd\ (common, compress, decompress) into zstd.lib.
$zsrc = Join-Path $repo 'src\externals\zstd'
$zstd = Join-Path $PSScriptRoot 'zstd.lib'
if (!(Test-Path -LiteralPath $zstd)) {
    $obj = Join-Path $PSScriptRoot 'zstd-obj'
    New-Item -ItemType Directory -Force $obj | Out-Null
    $c = Get-ChildItem (Join-Path $zsrc 'common\*.c'), (Join-Path $zsrc 'compress\*.c'), (Join-Path $zsrc 'decompress\*.c'), (Join-Path $zsrc 'dictBuilder\*.c')
    & (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /c /O2 /MD /W0 /DZSTD_DISABLE_ASM /clang:-march=x86-64-v3 $c.FullName "/Fo$obj\"
    if ($LASTEXITCODE -eq 0) { & (Join-Path $env:LLVM_ROOT 'bin\llvm-lib.exe') /nologo "/out:$zstd" (Get-ChildItem (Join-Path $obj '*.obj')).FullName }
    if ($LASTEXITCODE -ne 0) { Write-Error "zstd BUILD FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
}
# The GCN translator under test (phase 2, `xlat`) compiles with shaderc and validates with SPIRV-Tools from the Vulkan SDK.
$vk = "$env:GOW3_SDK_DIR\VulkanSDK"
# Same CPU baseline as the game build (CMakeLists.txt), so offline timings run the code the game runs.
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /MD /std:c++latest /EHsc /utf-8 /W3 /Z7 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS -fuse-ld=lld `
    /clang:-march=x86-64-v3 /clang:-ffp-contract=off $src `
    (Join-Path $repo 'src\gpu\shaders\gcn_translator.cpp') (Join-Path $repo 'src\gpu\shaders\gcn_spirv.cpp') (Join-Path $repo 'src\gpu\shaders\shader_runtime.cpp') (Join-Path $repo 'src\gpu\shaders\shader_jit.cpp') `
    (Join-Path $repo 'src\gpu\shaders\shader_aot.cpp') (Join-Path $repo 'src\gpu\shaders\shader_cache.cpp') (Join-Path $repo 'src\gpu\shaders\shader_parser.cpp') (Join-Path $repo 'src\gpu\shaders\shader_catalog.cpp') (Join-Path $repo 'src\gpu\shaders\shader_contract.cpp') (Join-Path $repo 'src\gpu\shaders\shader_recipes.cpp') (Join-Path $repo 'src\externals\xxhash\xxhash.c') `
    (Join-Path $repo 'src\gpu\vk_gpu.cpp') (Join-Path $repo 'src\gpu\device.cpp') (Join-Path $repo 'src\gpu\hazards.cpp') `
    (Join-Path $repo 'src\gpu\textures.cpp') (Join-Path $repo 'src\gpu\buffers.cpp') (Join-Path $repo 'src\gpu\piece_hash.cpp') (Join-Path $repo 'src\debug\render_probe.cpp') (Join-Path $PSScriptRoot 'offline_device.cpp') /DFMT_HEADER_ONLY /DGOW3_SPIRV_OPT /DGOW3_OFFLINE `
    "/I$(Join-Path $repo 'src')" "/I$vk\Include" "/I$(Join-Path $repo 'src\externals\fmt\include')" `
    "/I$(Join-Path $repo 'src\externals\magic_enum\include')" `
    "/I$(Join-Path $repo 'src\externals\xxhash')" "/I$zsrc" `
    "/Fe$(Join-Path $PSScriptRoot 'framecap.exe')" "/Fo$PSScriptRoot\" /link /DEBUG $zstd "$vk\Lib\shaderc_combined.lib" "$vk\Lib\vulkan-1.lib"
if ($LASTEXITCODE -ne 0) { Write-Error "BUILD FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
'BUILD SUCCEEDED: framecap.exe'
