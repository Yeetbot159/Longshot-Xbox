# How much memory Longshot takes on a PC, as the Xbox would count it: the process's committed (private) memory plus
# what it holds on the GPU (on the console both come out of one budget of about 5 GB). Runs the game without a window
# on a copy of a game folder, with its own user folder, presses A through the title and a new game's intro, samples
# every two seconds and ends the game after -Seconds.
#
#   powershell -ExecutionPolicy Bypass -File xbox\tools\memfit.ps1 -Game D:\Longshot-Xbox-work\memtest -Tag m4096 `
#       -MemoryMB 4096 -VramMB 1024 -Seconds 180
#
# Leaves runs\<tag>\ beside the game folder: samples.csv, summary.txt, the game's log and pictures (shots\).
param(
    [Parameter(Mandatory)] [string]$Game,
    [Parameter(Mandatory)] [string]$Tag,
    [int]$MemoryMB = 0,
    [int]$VramMB = 0,
    [int]$Seconds = 180,
    [int]$Preset = 1,
    [string]$Output = '1280x720',
    [string]$Presses = '',
    [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'

$runs = Join-Path (Split-Path $Game) 'runs'
$run = Join-Path $runs $Tag
Remove-Item -Recurse -Force $run -ErrorAction SilentlyContinue
$user = Join-Path $run 'user'
New-Item -ItemType Directory -Force $user, (Join-Path $run 'shots') | Out-Null
"quality_preset=$Preset`noutput_res=$Output`n" | Set-Content -Encoding ascii (Join-Path $user 'longshot_options.ini')

if (-not $Presses) {   # (A every 1.5 s: past the title, New Game, through the intro's text)
    $Presses = ((4..($Seconds - 5) | Where-Object { ($_ * 2) % 3 -eq 0 }) | ForEach-Object { "$($_):A" }) -join ','
}

$vars = @{
    HOS_HEADLESS = '1'; HOS_AUDIO_MUTE = '1'; HOS_DISCORD = '0'; HOS_CRASH_WATCH = '0'
    RX_VK_PRECOMPILE = '0'; HOS_OUTPUT_RES = $Output
    HOS_INPUT_SCRIPT = $Presses; HOS_INPUT_EXCLUSIVE = '1'; HOS_INPUT_BACKGROUND = '1'
    RX_VK_SHOTS = (Join-Path $run 'shots'); RX_VK_SHOT_EVERY = '600'
}
if ($MemoryMB) { $vars.HOS_MEMORY_MB = "$MemoryMB" }
if ($VramMB) { $vars.RX_VK_VRAM_MB = "$VramMB" }
foreach ($k in $Env.Keys) { $vars[$k] = $Env[$k] }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = Join-Path $Game 'bin\longshot.exe'
$psi.Arguments = "--user `"$user`""
$psi.WorkingDirectory = $run
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
$vars.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" } | Set-Content (Join-Path $run 'env-used.txt')

$p = [System.Diagnostics.Process]::Start($psi)
$log = Join-Path $run 'longshot_log.txt'
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()

function Get-GpuMB([int]$procId, [string]$kind) {
    try {
        $c = Get-Counter "\GPU Process Memory(pid_${procId}_*)\$kind" -ErrorAction Stop
        [int](($c.CounterSamples | Measure-Object CookedValue -Sum).Sum / 1MB)
    } catch { 0 }
}

$csv = Join-Path $run 'samples.csv'
'seconds,private_mb,working_set_mb,gpu_dedicated_mb,gpu_shared_mb,total_mb' | Set-Content $csv
$start = Get-Date
$peak = @{ private = 0; ws = 0; ded = 0; shared = 0; total = 0 }
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 2
    try { $p.Refresh() } catch { break }
    if ($p.HasExited) { break }
    $priv = [int]($p.PrivateMemorySize64 / 1MB)
    $ws = [int]($p.WorkingSet64 / 1MB)
    $ded = Get-GpuMB $p.Id 'Dedicated Usage'
    $sh = Get-GpuMB $p.Id 'Shared Usage'
    $tot = $priv + $ded
    $t = [int]((Get-Date) - $start).TotalSeconds
    "$t,$priv,$ws,$ded,$sh,$tot" | Add-Content $csv
    if ($priv -gt $peak.private) { $peak.private = $priv }
    if ($ws -gt $peak.ws) { $peak.ws = $ws }
    if ($ded -gt $peak.ded) { $peak.ded = $ded }
    if ($sh -gt $peak.shared) { $peak.shared = $sh }
    if ($tot -gt $peak.total) { $peak.total = $tot }
}
$exited = $p.HasExited
if (-not $exited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(10000) | Out-Null }
($outTask.Result + $errTask.Result) | Set-Content -Encoding utf8 $log

$shots = (Get-ChildItem (Join-Path $run 'shots') -Filter *.png -ErrorAction SilentlyContinue).Count
$summary = "tag=$Tag memory_mb=$(if ($MemoryMB) { $MemoryMB } else { 'default' }) vram_mb=$(if ($VramMB) { $VramMB } else { 'default' }) " +
    "preset=$Preset output=$Output seconds=$Seconds exit=$(if ($exited) { $p.ExitCode } else { 'ended-by-script' }) " +
    "peak_private_mb=$($peak.private) peak_working_set_mb=$($peak.ws) peak_gpu_dedicated_mb=$($peak.ded) " +
    "peak_gpu_shared_mb=$($peak.shared) peak_private_plus_dedicated_mb=$($peak.total) shots=$shots"
$summary | Set-Content (Join-Path $run 'summary.txt')
$summary
