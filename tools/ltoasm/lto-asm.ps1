$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Build = "$Root\build-fast\RelWithDebInfo"
$Work = "$Root\build-fast\asm"
foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$libraries = @('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }
$env:LIB = (@($libraries) + $env:LIB) -join ';'

# Objects of the gow3 target, copied so the saved temps land beside the copies and not in the build tree.
$ninja = Get-Content -LiteralPath "$Build\build.ninja" | Where-Object { $_.StartsWith('build bin\gow3.exe:') }
$objs = [regex]::Matches($ninja, 'CMakeFiles\\gow3\.dir\\\S+\.obj') | ForEach-Object Value
Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Work | Out-Null
foreach ($o in $objs) {
    $dst = Join-Path $Work $o
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item -LiteralPath (Join-Path $Build $o) -Destination $dst
}
$libs = (Select-String -LiteralPath "$Build\build.ninja" -Pattern '^  LINK_LIBRARIES = ' -Context 0, 0 |
    Where-Object { $_.LineNumber -gt 1919 } | Select-Object -First 1).Line -replace '^  LINK_LIBRARIES = ', ''
$libs = ($libs -split '\s+' | Where-Object { $_ }) | ForEach-Object {
    if ($_ -like 'lib\*') { "$Build\$_" } else { $_ }
}
$rsp = @($objs) + $libs
Set-Content -LiteralPath "$Work\gow3.rsp" -Value $rsp
Set-Location $Work
New-Item -ItemType Directory -Force "$Work\out" | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
& (Join-Path $env:LLVM_ROOT 'bin\lld-link.exe') /nologo '@gow3.rsp' /out:out\gow3.exe /pdb:out\gow3.pdb /machine:x64 /debug `
    /subsystem:windows /lldsavetemps:precodegen "/lldltocache:$Work\cache" 2>&1 | Select-Object -First 40
Write-Output "LINK_EXIT=$LASTEXITCODE elapsed=$($sw.Elapsed)"
