# GPU utilization per process and engine (same source as Task Manager), averaged over -Seconds.
# Run while the game is in gameplay; compare with -DisableLayers launches (VK_LOADER_LAYERS_DISABLE=~implicit~).
param([int]$Seconds = 10)
# Engine instances come and go while the game runs; such samples are invalid, so average over the valid ones only.
$samples = @(Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples $Seconds -ErrorAction SilentlyContinue)
$valid = $samples.Count
"valid samples: $valid of $Seconds"
if (!$valid) { exit 1 }
$names = @{}
Get-Process | ForEach-Object { $names[$_.Id] = $_.ProcessName }
$samples.CounterSamples | Where-Object { $_.Status -eq 0 -and $_.CookedValue -gt 0 } | ForEach-Object {
    if ($_.InstanceName -match 'pid_(\d+)_.*engtype_(.+)$') {
        [pscustomobject]@{ Process = "$($names[[int]$Matches[1]])($($Matches[1]))"; Engine = $Matches[2]; Value = $_.CookedValue }
    }
} | Group-Object Process, Engine | ForEach-Object {
    [pscustomobject]@{ Process = $_.Group[0].Process; Engine = $_.Group[0].Engine
                       AvgPercent = [math]::Round(($_.Group | Measure-Object Value -Sum).Sum / $valid, 1) }
} | Sort-Object AvgPercent -Descending | Format-Table -AutoSize
# NVIDIA's own view (SM busy %, power), plus the overlay/layer DLLs injected into the game.
nvidia-smi dmon -c 5 -s pu
$game = Get-Process gow3 -ErrorAction SilentlyContinue
if ($game) {
    "layers/hooks in gow3: " + (($game.Modules | Where-Object ModuleName -match 'Layer|Hooks|obs|Overlay|renderdoc|nomad|ngfx' |
        ForEach-Object ModuleName) -join ', ')
}
