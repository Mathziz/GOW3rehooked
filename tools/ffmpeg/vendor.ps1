# Vendors into src/externals/ffmpeg the FFmpeg sources the port links, which the main CMake build compiles as the ffmpeg
# target: the H.264 decoder (cinematics) and the MP3 decoder + parser and swresample (sceAjm). Run only to change versions.
# It configures and builds FFmpeg once (Git Bash + MSYS2 make, fetched and SHA-512 checked), links probe.c with /MAP to
# find the objects the linker loads, then copies their sources, the headers they include (.d files) and configure's output.
[CmdletBinding()]
param([string]$Version = '9.0.2', [switch]$Reuse, [ValidateRange(1, 256)][int]$Jobs = [Environment]::ProcessorCount)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Work = Join-Path $Root 'build-fast\ffmpeg'
$Target = Join-Path $Root 'src\externals\ffmpeg'
New-Item -ItemType Directory -Path $Work -Force | Out-Null

foreach ($line in Get-Content -LiteralPath "$env:GOW3_SDK_DIR\EWDK\EWDK-LLVM.env") {
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $name, $value = $line -split '=', 2
    if ($name -notin @('HOME', 'CODEX_HOME')) { Set-Item -LiteralPath "Env:$name" -Value $value }
}
$env:INCLUDE = (@('ucrt', 'shared', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Include\$($env:Version_Number)\$_" }) + $env:INCLUDE -join ';'
$env:LIB = (@('ucrt', 'um') | ForEach-Object { Join-Path $env:WindowsSdkDir "Lib\$($env:Version_Number)\$_\x64" }) + $env:LIB -join ';'

$source = Join-Path $Work "src-$Version"
$objects = Join-Path $Work "obj-$Version"
if (-not ($Reuse -and (Test-Path -LiteralPath (Join-Path $objects 'libavcodec\avcodec.lib')))) {
    $make = Join-Path $Work 'make\usr\bin\make.exe'
    if (-not (Test-Path -LiteralPath $make)) {
        $package = Join-Path $Work 'make-4.4.1-2-x86_64.pkg.tar.zst'
        curl.exe -fsSL -o $package 'https://mirror.msys2.org/msys/x86_64/make-4.4.1-2-x86_64.pkg.tar.zst' # Invoke-WebRequest gets a bot check
        $expected = 'B55CAAF0D54B784B5DFFCBB75A1862FC7359B91CAA1E60234E208DE03C74159FD003D68F5DDDD387ADEF752BB13805E654F17EC5CB6ADD51546B9E30A1091B61'
        if ((Get-FileHash -LiteralPath $package -Algorithm SHA512).Hash -ne $expected) { throw 'make package checksum mismatch' }
        New-Item -ItemType Directory -Path (Join-Path $Work 'make') -Force | Out-Null
        & "$env:SystemRoot\System32\tar.exe" -xf $package -C (Join-Path $Work 'make') usr/bin/make.exe
        if ($LASTEXITCODE) { throw 'make extraction failed' }
    }
    if (-not (Test-Path -LiteralPath $source)) {
        git -c advice.detachedHead=false clone --depth 1 --branch "n$Version" https://github.com/FFmpeg/FFmpeg.git $source
        if ($LASTEXITCODE) { throw 'FFmpeg clone failed' }
    }
    Remove-Item -LiteralPath $objects -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $objects -Force | Out-Null
    $options = @(
        '--toolchain=msvc', '--cc=clang-cl', '--cxx=clang-cl', '--ld=lld-link', '--target-os=win64', '--arch=x86_64',
        '--enable-static', '--disable-shared', '--disable-programs', '--disable-doc', '--disable-debug', '--disable-network',
        '--disable-autodetect', '--enable-w32threads', '--disable-everything', '--disable-avformat', '--disable-avfilter',
        '--disable-avdevice', '--disable-swscale', '--disable-error-resilience', '--disable-faan', '--disable-pixelutils',
        '--disable-avx512', '--disable-avx512icl', '--enable-decoder=h264', '--enable-decoder=mp3', '--enable-parser=mpegaudio',
        "--extra-cflags='-MT -arch:AVX2'"
    ) -join ' '
    $git = Split-Path (Split-Path (Get-Command git).Source)
    $script = @"
set -e
export PATH="/usr/bin:`$(cygpath -u '$(Split-Path $make)'):`$(cygpath -u '$env:GOW3_SDK_DIR\mingw64\bin'):`$(cygpath -u '$env:LLVM_ROOT\bin'):`$PATH"
cd "`$(cygpath -u '$objects')"
"`$(cygpath -u '$source')/configure" $options
make -j$Jobs
"@
    & (Join-Path $git 'usr\bin\bash.exe') -c $script
    if ($LASTEXITCODE) { throw "FFmpeg build failed with exit code $LASTEXITCODE" }
}

Push-Location $objects
try {
    $probe = Join-Path $PSScriptRoot 'probe.c'
    $includes = & "$env:LLVM_ROOT\bin\clang-cl.exe" -nologo -MT -Zs -showIncludes "-I$objects" "-I$source" $probe
    & "$env:LLVM_ROOT\bin\clang-cl.exe" -nologo -MT -c "-I$objects" "-I$source" $probe "-Fo$objects\probe.obj"
    if ($LASTEXITCODE) { throw 'probe compile failed' }
    # The members the linker loads, before unreferenced functions are dropped (the game's debug link keeps them all). They
    # are named lib(basename); x86/ twins of a name are both taken.
    $log = & "$env:LLVM_ROOT\bin\lld-link.exe" -nologo -verbose probe.obj libavcodec\avcodec.lib libswresample\swresample.lib libavutil\avutil.lib bcrypt.lib -out:probe.exe 2>&1
    if ($LASTEXITCODE) { throw 'probe link failed' }
    $taken = $log | Select-String 'Loaded (avcodec|avutil|swresample)\.lib\(([^)]+\.o)\)' | ForEach-Object { "lib$($_.Matches[0].Groups[1].Value)|$($_.Matches[0].Groups[2].Value)" }
    $taken = [Collections.Generic.HashSet[string]]::new([string[]]@($taken))
    $linked = Get-ChildItem -LiteralPath 'libavcodec', 'libavutil', 'libswresample' -Recurse -Filter *.o |
        Where-Object { $taken.Contains("$($_.FullName.Substring($objects.Length + 1).Split('\')[0])|$($_.Name)") } |
        ForEach-Object { $_.FullName.Substring($objects.Length + 1) -replace '\\', '/' } | Sort-Object

    $files = [Collections.Generic.HashSet[string]]::new()
    $compiled = foreach ($object in $linked) {
        $stem = $object -replace '\.o$', ''
        $file = if (Test-Path -LiteralPath (Join-Path $source "$stem.c")) { "$stem.c" } else { "$stem.asm" }
        [void]$files.Add($file)
        $file
        # C: "x.o: header" per line (paths with spaces are left out); NASM: "x.o : \" then several per line.
        $dependencies = (Get-Content -LiteralPath "$stem.d" -Raw) -replace '(?m)^\S+\.o\s*:', ''
        foreach ($dependency in $dependencies -split '\s+') { if ($dependency -and $dependency -ne '\') { [void]$files.Add($dependency) } }
    }
    foreach ($line in $includes) { if ($line -match 'including file:\s*(.+)$') { [void]$files.Add($Matches[1].Trim()) } }
} finally { Pop-Location }

Remove-Item -LiteralPath $Target -Recurse -Force -ErrorAction SilentlyContinue
$sourceRoot = (Resolve-Path $source).Path.Replace('\', '/') + '/'
$objectRoot = (Resolve-Path $objects).Path.Replace('\', '/') + '/'
$copied = 0
foreach ($file in $files) {
    $path = ($file -replace '\\', '/') -replace '^\./', ''
    if ($path.StartsWith($sourceRoot, 'OrdinalIgnoreCase')) { $relative = $path.Substring($sourceRoot.Length); $from = Join-Path $source $relative }
    elseif ($path.StartsWith($objectRoot, 'OrdinalIgnoreCase')) { $relative = $path.Substring($objectRoot.Length); $from = Join-Path $objects $relative }
    elseif ($path -notmatch ':') { $relative = $path; $from = if (Test-Path -LiteralPath (Join-Path $objects $path)) { Join-Path $objects $path } else { Join-Path $source $path } }
    else { continue } # compiler and SDK headers
    $to = Join-Path $Target $relative
    New-Item -ItemType Directory -Path (Split-Path $to) -Force | Out-Null
    Copy-Item -LiteralPath $from -Destination $to
    ++$copied
}
Copy-Item -LiteralPath (Join-Path $source 'COPYING.LGPLv2.1'), (Join-Path $source 'LICENSE.md') -Destination $Target
$list = ($compiled | ForEach-Object { "    $_" }) -join "`n"
@"
# FFmpeg $Version, vendored by tools/ffmpeg/vendor.ps1: the sources of the objects the port links (H.264 decoder for the
# cinematics, MP3 decoder + parser and swresample for sceAjm) and configure's output (config.h, config_components.h, config.asm).
find_program(NASM nasm REQUIRED)
set(CMAKE_ASM_NASM_COMPILER `${NASM})
enable_language(ASM_NASM)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "") # the global clang-cl flags would reach NASM
add_library(ffmpeg STATIC
$list
)
target_include_directories(ffmpeg SYSTEM PUBLIC `${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ffmpeg PRIVATE `$<`$<COMPILE_LANGUAGE:C>:HAVE_AV_CONFIG_H;_ISOC11_SOURCE;_FILE_OFFSET_BITS=64;_LARGEFILE_SOURCE;_USE_MATH_DEFINES;_CRT_NONSTDC_NO_WARNINGS;_WIN32_WINNT=0x0600;PIC>)
# FFmpeg relies on the optimizer dropping calls under disabled ARCH_*/HAVE_* checks; no LTO (inline asm names symbols).
target_compile_options(ffmpeg PRIVATE `$<`$<COMPILE_LANGUAGE:C>:/clang:-march=x86-64-v3;/std:c17;-w>
    `$<`$<COMPILE_LANGUAGE:ASM_NASM>:-I`${CMAKE_CURRENT_SOURCE_DIR}/;-I`${CMAKE_CURRENT_SOURCE_DIR}/libavutil/x86/;-P`${CMAKE_CURRENT_SOURCE_DIR}/config.asm>)
target_link_libraries(ffmpeg PUBLIC bcrypt)
"@ | Set-Content -LiteralPath (Join-Path $Target 'CMakeLists.txt')
Write-Host "$($compiled.Count) sources, $copied files in $Target"
