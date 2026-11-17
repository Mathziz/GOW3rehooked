# Starts shadPS4 and samples it. -Ranges: label hexaddr hexsize per line. -Scan: whole-memory search, -Patterns: files to look for.
param([string]$Ranges = '', [string]$Dir = (Join-Path $PSScriptRoot '..\..\build-fast\lightcap\shad'), [int]$Seconds = 25, [switch]$Scan, [string[]]$Patterns = @())
$emu = $env:GOW3_SHADPS4_DIR; $log = "$emu\user\log\shad_log.txt"  # shadPS4's user folder; GOW3_SHADPS4_EXE its executable
New-Item -ItemType Directory -Force $Dir | Out-Null
$t = Get-Date
$p = Start-Process $env:GOW3_SHADPS4_EXE -ArgumentList '-g', "$env:GOW3_GAME_DIR\eboot.bin" -WorkingDirectory $emu -PassThru
$base = $null
for ($i = 0; $i -lt 120 -and -not $base; $i++) {
    Start-Sleep 1
    if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t) {
        $line = Get-Content $log -TotalCount 400 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'Loading module eboot\.bin to (0x[0-9a-fA-F]+)' } | Select-Object -First 1
        if ($line -match 'to (0x[0-9a-fA-F]+)') { $base = $Matches[1] }
    }
}
"process $($p.Id) exited=$($p.HasExited) base=$base"
if (!$base) { exit 1 }
$extra = @(); if ($Ranges -or $Scan) { $extra = @($(if ($Ranges) { $Ranges } else { 'NUL' }), $Dir) }; if ($Scan) { $extra += @('scan') + $Patterns }
& "$PSScriptRoot\lightcap.exe" 'shadps4.exe' $base.Substring(2) "$Dir\lights.txt" $Seconds @extra
"lightcap exit $LASTEXITCODE"
if (!$p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep 3; if (!$p.HasExited) { Stop-Process -Id $p.Id -Force } }
