# Builds xferbench.exe with the EWDK clang-cl toolchain; the compute shader is compiled by the Vulkan SDK glslc.
$ErrorActionPreference = 'Stop'
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = ((@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE) -join ';'
$env:LIB = ((@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB) -join ';'
$sdk = "$env:GOW3_SDK_DIR\VulkanSDK"
& "$sdk\Bin\glslc.exe" --target-env=vulkan1.3 -O -mfmt=num -o (Join-Path $PSScriptRoot 'xfer.comp.inc') (Join-Path $PSScriptRoot 'xfer.comp')
if ($LASTEXITCODE -ne 0) { Write-Error "SHADER FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
& (Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe') /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W3 /Z7 -fuse-ld=lld (Join-Path $PSScriptRoot 'xferbench.cpp') `
    "/I$sdk\Include" "/I$PSScriptRoot" "/Fe$(Join-Path $PSScriptRoot 'xferbench.exe')" "/Fo$(Join-Path $PSScriptRoot 'xferbench.obj')" `
    /link /DEBUG "$sdk\Lib\vulkan-1.lib"
if ($LASTEXITCODE -ne 0) { Write-Error "BUILD FAILED ($LASTEXITCODE)" -ErrorAction Continue; exit 1 }
'BUILD SUCCEEDED: xferbench.exe'
