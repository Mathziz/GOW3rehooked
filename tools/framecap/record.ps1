# Records every frame from launch to exit: starts `framecap record` (it waits for the new gow3.exe), launches the game
# through debug.ps1 and, once the game closes, waits for the recorder to flush. Ctrl+C stops both cleanly.
param([string]$Out = (Join-Path $PSScriptRoot "..\..\build-fast\framecap\rec\boot-$(Get-Date -Format yyyyMMdd-HHmmss).rec"))
$ErrorActionPreference = 'Stop'
$Out = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
$exe = Join-Path $PSScriptRoot 'framecap.exe'
$rec = Start-Process -FilePath $exe -ArgumentList 'record', "`"$Out`"" -NoNewWindow -PassThru
$null = $rec.Handle  # keeps the exit code readable after exit
# The recorder must note any gow3.exe already running before the new one starts.
Start-Sleep -Milliseconds 500
if ($rec.HasExited) { throw "framecap exited early ($($rec.ExitCode))" }
& (Join-Path $PSScriptRoot '..\..\debug.ps1')
$rec.WaitForExit()
"recording: $Out"
"timeline : & '$exe' timeline '$Out' '$([IO.Path]::ChangeExtension($Out, '.csv'))'"
exit $rec.ExitCode
