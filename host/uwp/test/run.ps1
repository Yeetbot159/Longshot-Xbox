# Build and run host/uwp's test on a PC with llvm-mingw, as the game module is built, linked against the UWP import
# library only (libwindowsapp.a: no user32, winmm or shell32).
#     powershell -ExecutionPolicy Bypass -File host\uwp\test\run.ps1 [-Mingw <llvm-mingw folder>]
param([string]$Mingw = $(if ($env:LONGSHOT_MINGW) { $env:LONGSHOT_MINGW } else { 'D:\longshot-tools\llvm-mingw-20261104-ucrt-x86_64' }))
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here '..\..\..\xbox\out\uwp-test'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = Join-Path $Mingw 'bin\clang.exe'
if (-not (Test-Path $clang)) { throw "no llvm-mingw at $Mingw (pass -Mingw or set LONGSHOT_MINGW)" }
$exe = Join-Path $out 'test_win32_uwp.exe'
& $clang --target=x86_64-w64-mingw32 -O1 -Wall -Wextra -Werror -Wno-unused-parameter `
    (Join-Path $here 'test_win32_uwp.c') (Join-Path $here '..\win32_uwp.c') (Join-Path $here '..\uwp_entry.c') `
    '-Wl,-Xlink=-ignore:4217' -nodefaultlibs '-lmingw32' '-lmingwex' '-lucrt' '-lwindowsapp' '-lclang_rt.builtins-x86_64' -o $exe
if ($LASTEXITCODE) { throw 'test_win32_uwp.exe did not build' }
& $exe
exit $LASTEXITCODE
