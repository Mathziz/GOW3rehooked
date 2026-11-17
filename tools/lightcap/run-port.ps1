# Starts the port through debug.ps1 and samples it. -GameEnv: NAME=VALUE,NAME=VALUE for the game; the rest as in run-shad.ps1.
param([string]$Ranges = '', [string]$Dir = (Join-Path $PSScriptRoot '..\..\build-fast\lightcap\port'), [int]$Seconds = 25, [switch]$Scan, [string[]]$Patterns = @(), [string]$GameEnv = '')
$log = "$env:GOW3_GAME_DIR\_rehooked_\gow3.log"
New-Item -ItemType Directory -Force $Dir | Out-Null
$n0 = (Get-Content $log | Measure-Object -Line).Lines
$arguments = @('-NoProfile', '-File', (Resolve-Path "$PSScriptRoot\..\..\debug.ps1").Path); if ($GameEnv) { $arguments += @('-Env', $GameEnv) }
Start-Process pwsh -ArgumentList $arguments -WindowStyle Hidden
$base = $null
for ($i = 0; $i -lt 90 -and -not $base; $i++) {
    Start-Sleep 1
    if ((Get-Content $log | Measure-Object -Line).Lines -le $n0) { continue }
    $line = Get-Content $log -Tail 6000 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'Mapped eboot\.bin base=(0x[0-9a-fA-F]+)' } | Select-Object -Last 1
    if ($line -match 'base=(0x[0-9a-fA-F]+)') { $base = $Matches[1] }
}
"base=$base"
if (!$base) { exit 1 }
$extra = @(); if ($Ranges -or $Scan) { $extra = @($(if ($Ranges) { $Ranges } else { 'NUL' }), $Dir) }; if ($Scan) { $extra += @('scan') + $Patterns }
& "$PSScriptRoot\lightcap.exe" 'gow3.exe' $base.Substring(2) "$Dir\lights.txt" $Seconds @extra
"lightcap exit $LASTEXITCODE"
Get-Process gow3 -ErrorAction SilentlyContinue | Stop-Process -Force
