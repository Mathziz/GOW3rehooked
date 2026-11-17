param([string]$Name = 'cpu', [int]$Seconds = 10, [string]$Profiles = 'CPU')
# Elevated: wpr in memory mode, fixed window, transcript instead of piping wpr's console.
$logs = "$env:GOW3_GAME_DIR\_rehooked_"
$etl = Join-Path $logs "$Name.etl"
if (Test-Path -LiteralPath $etl) { $etl = Join-Path $logs ("$Name-" + (Get-Date -Format 'HHmmss') + '.etl') }
Start-Transcript -Path (Join-Path $logs "$Name-wpr.txt") -Force | Out-Null
wpr -cancel 2>$null
wpr @(@($Profiles -split ',') + "$PSScriptRoot\gow3.wprp!GOW3" | ForEach-Object { '-start', $_ })
Write-Output "capturing $Seconds s"
Start-Sleep $Seconds
wpr -stop $etl -skipPdbGen
Write-Output "exit=$LASTEXITCODE etl=$etl"
Stop-Transcript | Out-Null
