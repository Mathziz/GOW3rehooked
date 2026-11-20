# Live session for the renderer phases: records every frame and the draw shadow (GOW3_DRAWLOG) while you play, then
# checks the draw reader against the GPU stream (drawcheck) and translates every program seen (xlat). Close the game to
# finish, or pass -Seconds. Output: build-fast\framecap\live-<stamp>\.
param([int]$Seconds = 0)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$game = $env:GOW3_GAME_DIR
$fc = Join-Path $PSScriptRoot 'framecap.exe'
$dir = Join-Path $repo "build-fast\framecap\live-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$rec = Join-Path $dir 'session.rec'
$start = Get-Date
$r = Start-Process -FilePath $fc -ArgumentList 'record', "`"$rec`"", '--heap' -NoNewWindow -PassThru  # heap: offline replay needs meshes/textures
$null = $r.Handle
Start-Sleep -Milliseconds 500
$env:GOW3_DEBUG = '1'  # diagnostics on (the draw shadow is one)
$env:GOW3_DRAWLOG = '1'
$g = Start-Process -FilePath "$game\gow3.exe" -WorkingDirectory $game -PassThru
if ($Seconds) { $null = $g.WaitForExit($Seconds * 1000) } else { $g.WaitForExit() }
if (!$g.HasExited) { $null = $g.CloseMainWindow(); if (!$g.WaitForExit(8000)) { Stop-Process -Id $g.Id -Force } }
$r.WaitForExit()
$log = Get-ChildItem "$game\_rehooked_\draws-*.bin" -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $start | Sort-Object LastWriteTime | Select-Object -Last 1
$zip = Get-ChildItem "$game\_rehooked_\gow3-*.zip" -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $start | Sort-Object LastWriteTime | Select-Object -Last 1
$dump = $null
if (!$log -and $zip) {  # the session's .dmp, stored whole in gow3-<stamp>.zip or in pieces .dmp.001... across its .partN.zip files
    $stem = $zip.Name -replace '(\.part\d+)?\.zip$'
    Get-ChildItem "$game\_rehooked_\$stem.zip", "$game\_rehooked_\$stem.part*.zip" -ErrorAction SilentlyContinue | ForEach-Object { & tar -xf $_.FullName -C $dir }
    $dump = Get-Item (Join-Path $dir "$stem.dmp") -ErrorAction SilentlyContinue
    $pieces = Get-ChildItem (Join-Path $dir "$stem.dmp.*") -ErrorAction SilentlyContinue | Sort-Object Name
    if (!$dump -and $pieces) {
        $out = [IO.File]::Create((Join-Path $dir "$stem.dmp"))
        foreach ($p in $pieces) { $in = [IO.File]::OpenRead($p.FullName); $in.CopyTo($out); $in.Dispose() }
        $out.Dispose()
        $dump = Get-Item (Join-Path $dir "$stem.dmp")
    }
}
if (!$log -and $dump) {
    $entry = (& tar -tf $dump.FullName 2>$null | Where-Object { $_ -match '^draws-.*\.bin$' } | Sort-Object | Select-Object -Last 1)
    if ($entry) {
        & tar -xf $dump.FullName -C $dir $entry
        if ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $dir $entry))) { $log = Get-Item (Join-Path $dir $entry) }
    }
}
if (!$log) { Write-Error 'no draw log from this session' -ErrorAction Continue; exit 1 }
"recording $rec ($([math]::Round((Get-Item $rec).Length / 1MB)) MB), draw log $($log.FullName) ($([math]::Round($log.Length / 1MB)) MB)"
& $fc drawcheck $rec $log.FullName (Join-Path $dir 'drawcheck.txt')
& $fc xlat $rec (Join-Path $dir 'xlat') --light
"`n--- drawcheck"
Get-Content (Join-Path $dir 'drawcheck.txt') -TotalCount 12
"`n--- xlat"
Get-Content (Join-Path $dir 'xlat\report.txt')
