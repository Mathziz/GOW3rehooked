# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$GameDirectory, [ValidateRange(1, 2)][int]$Level = 1, [switch]$Vulkan, [int]$DumpStop = -1, [string[]]$Env = @(),
      [ValidateSet('trace', 'debug', 'info', 'warning', 'error', 'critical', 'off')][string]$LogLevel)
$ErrorActionPreference = 'Stop'
if (!$GameDirectory) {
    # Deployed in <game>\_rehooked_\tools.
    $scriptGame = if ((Split-Path -Leaf (Split-Path -Parent $PSScriptRoot)) -eq '_rehooked_') { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) } else { $PSScriptRoot }
    $GameDirectory = if (Test-Path -LiteralPath (Join-Path $scriptGame 'gow3.exe')) { $scriptGame } else { $env:GOW3_GAME_DIR }
    if (!$GameDirectory) { throw 'Pass -GameDirectory or set GOW3_GAME_DIR to the game folder' }
}
$GameDirectory = (Resolve-Path -LiteralPath $GameDirectory).Path
$executable = Join-Path $GameDirectory 'gow3.exe'
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) { throw "gow3.exe not found: $executable" }
$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = (Resolve-Path -LiteralPath $executable).Path
$start.WorkingDirectory = Split-Path -Parent $start.FileName
$start.UseShellExecute = $false
# Change only the child's environment, including inherited fault/dump experiments.
foreach ($name in @($start.EnvironmentVariables.Keys)) { if ($name -like 'GOW3_*') { $start.EnvironmentVariables.Remove($name) } }
# GOW3_DEBUG=1 turns on standard diagnostics; GOW3_DEBUG=2 enables deep diagnostics (textures, Vulkan validation, S4, stats, prof, drawlog...).
$start.EnvironmentVariables['GOW3_DEBUG'] = "$Level"
# F9 frame dump: the dumped frame stops after this draw (its targets show the intermediate state).
if ($DumpStop -ge 0) { $start.EnvironmentVariables['GOW3_DUMP_STOP'] = "$DumpStop" }
# Orbis HLE log level (default info: the per-frame trace and debug lines are dropped before formatting); -LogLevel trace
# brings every call back, and each stub logs every call instead of its first.
if ($LogLevel) { $start.EnvironmentVariables['GOW3_LOG_LEVEL'] = $LogLevel }
# Experiment toggles: -Env NAME=VALUE,NAME=VALUE.
foreach ($v in $Env -split ',') { $name, $value = $v -split '=', 2; $start.EnvironmentVariables[$name] = $value }
# Khronos validation from the Vulkan SDK, logged to gow3.log (also active in -Level 2 unless GOW3_VK_VALIDATE=0).
if ($Vulkan -or $Level -ge 2) {
    if ($Vulkan) { $start.EnvironmentVariables['GOW3_VK_VALIDATE'] = '1' }
    $start.EnvironmentVariables.Remove('VK_LOADER_LAYERS_DISABLE')
    $layers = Join-Path $(if ($env:VULKAN_SDK) { $env:VULKAN_SDK } else { "$env:GOW3_SDK_DIR\VulkanSDK" }) 'Bin'
    if (Test-Path -LiteralPath (Join-Path $layers 'VkLayer_khronos_validation.json')) { $start.EnvironmentVariables['VK_LAYER_PATH'] = $layers }
}
$process = [System.Diagnostics.Process]::Start($start)
try { $process.WaitForExit(); $code = $process.ExitCode } finally { $process.Dispose() }
exit $code
