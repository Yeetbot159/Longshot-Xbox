# Run the Xbox build of the game on this PC through the app's chain (pc_run.cpp): the game module, the Xbox renderer and
# Mesa's Dozen loaded by hand, Dozen presenting through the app's DComp.DLL. No window and no sound; the renderer's
# pictures go to runs\<tag>\shots beside the -Runs folder. Everything stays on this PC (CONTRIBUTING.md).
#
#   powershell -ExecutionPolicy Bypass -File xbox\app\test\pc_run.ps1 -Module <work>\sa\build\longshot_game.dll `
#       -Renderer <vk work>\sa\build\longshot_vk.dll -Romfs <dump>\romfs -Image <game>\image.bin -Tag first -Seconds 120
param(
    [Parameter(Mandatory)] [string]$Module,
    [Parameter(Mandatory)] [string]$Renderer,
    [Parameter(Mandatory)] [string]$Romfs,
    [string]$Image = '',
    [Parameter(Mandatory)] [string]$Tag,
    [int]$Seconds = 120,
    [string]$Runs = 'D:\Longshot-Xbox-work\runs-xbox',
    [string]$Size = '1280x720',
    [int]$Preset = 1,
    [string]$Presses = '',
    [string[]]$Env = @(),   # (NAME=VALUE)
    [string]$Mesa = $(if ($env:LONGSHOT_MESA) { $env:LONGSHOT_MESA } else { 'D:\longshot-tools\mesa-eval\x\x64' }),
    [string]$Mingw = $(if ($env:LONGSHOT_MINGW) { $env:LONGSHOT_MINGW } else { 'D:\longshot-tools\llvm-mingw-20261104-ucrt-x86_64' })
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\common.ps1')
Enter-VsEnv

$here = $PSScriptRoot
$out = Join-Path $XboxOut 'pc-run'
$pkg = Join-Path $out 'package'
New-Item -ItemType Directory -Force $out, $pkg | Out-Null
foreach ($dll in 'vulkan_dzn.dll', 'dxil.dll') { Copy-Item (Join-Path $Mesa $dll) $pkg }
foreach ($dll in 'libc++.dll', 'libunwind.dll') { Copy-Item (Join-Path $Mingw "bin\$dll") $pkg }
Copy-Item $Renderer (Join-Path $pkg 'longshot_vk.dll')

cl /nologo /O2 /MT /W4 /Fo"$out\\" (Join-Path $here '..\tls_slot.c') /LD /link /OUT:"$pkg\longshot_tls.dll" /IMPLIB:"$out\longshot_tls.lib"
if ($LASTEXITCODE) { throw 'longshot_tls.dll did not build' }
cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /Fo"$out\\" (Join-Path $here 'pc_run.cpp') (Join-Path $here '..\modules.cpp') `
    (Join-Path $here '..\dcomp_shim.cpp') (Join-Path $here '..\..\loader\pe_loader.c') /link /OUT:"$out\pc_run.exe"
if ($LASTEXITCODE) { throw 'pc_run.exe did not build' }

$run = Join-Path $Runs $Tag
Remove-Item -Recurse -Force $run -ErrorAction SilentlyContinue
$user = Join-Path $run 'user'
New-Item -ItemType Directory -Force $user, (Join-Path $run 'shots') | Out-Null
"quality_preset=$Preset`noutput_res=$Size`n" | Set-Content -Encoding ascii (Join-Path $user 'longshot_options.ini')
if (-not $Presses) {   # (A every 1.5 s: past the title, New Game, through the intro's text)
    $Presses = ((4..($Seconds - 5) | Where-Object { ($_ * 2) % 3 -eq 0 }) | ForEach-Object { "$($_):A" }) -join ','
}
$vars = @{
    HOS_AUDIO_MUTE = '1'; HOS_DISCORD = '0'; HOS_CRASH_WATCH = '0'; RX_VK_PRECOMPILE = '0'; HOS_OUTPUT_RES = $Size
    HOS_INPUT_SCRIPT = $Presses; HOS_INPUT_EXCLUSIVE = '1'; HOS_INPUT_BACKGROUND = '1'; PC_RUN_SIZE = $Size
    RX_VK_SHOTS = (Join-Path $run 'shots'); RX_VK_SHOT_EVERY = '300'
}
foreach ($kv in $Env) {
    $name, $value = $kv -split '=', 2
    $vars[$name] = $value
}
$vars.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" } | Set-Content (Join-Path $run 'env-used.txt')

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = Join-Path $out 'pc_run.exe'
$psi.Arguments = "`"$pkg`" `"$Module`" $Seconds `"$user`" `"$Romfs`"" + $(if ($Image) { " `"$Image`"" } else { '' })
$psi.WorkingDirectory = $run
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
$p = [System.Diagnostics.Process]::Start($psi)
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()
$peak = 0
while (-not $p.WaitForExit(2000)) {
    try { $p.Refresh(); $peak = [Math]::Max($peak, [int]($p.PrivateMemorySize64 / 1MB)) } catch {}
}
$p.WaitForExit()
$log = Join-Path $run 'longshot_log.txt'
($outTask.Result + $errTask.Result) | Set-Content -Encoding utf8 $log
$shots = (Get-ChildItem (Join-Path $run 'shots') -Filter *.png -ErrorAction SilentlyContinue).Count
$summary = "tag=$Tag seconds=$Seconds exit=$($p.ExitCode) peak_private_mb=$peak shots=$shots log=$log"
$summary | Set-Content (Join-Path $run 'summary.txt')
$summary
