# Build and run the loader's test: the modules with llvm-mingw (as the game module is built), the driver with MSVC (as
# the UWP host is).
#     powershell -ExecutionPolicy Bypass -File xbox\loader\test\run.ps1 [-Mingw <llvm-mingw folder>]
param([string]$Mingw = $(if ($env:LONGSHOT_MINGW) { $env:LONGSHOT_MINGW } else { 'D:\longshot-tools\llvm-mingw-20261104-ucrt-x86_64' }))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\common.ps1')
Enter-VsEnv

$here = $PSScriptRoot
$out = Join-Path $XboxOut 'loader-test'
New-Item -ItemType Directory -Force $out | Out-Null
$bin = Join-Path $Mingw 'bin'
if (-not (Test-Path (Join-Path $bin 'clang++.exe'))) { throw "no llvm-mingw at $Mingw (pass -Mingw or set LONGSHOT_MINGW)" }

& (Join-Path $bin 'llvm-dlltool.exe') -m i386:x86-64 -d (Join-Path $here 'host.def') -l (Join-Path $out 'libhost.a')
if ($LASTEXITCODE) { throw 'llvm-dlltool failed' }
# (not -static: libc++abi keeps its exception state in real thread-locals, which a module loaded by hand cannot have; the
# toolchain's runtime DLLs go in the package beside the host, and Windows loads those)
& (Join-Path $bin 'clang++.exe') --target=x86_64-w64-mingw32 -O2 -shared -femulated-tls `
    (Join-Path $here 'test_mod.cpp') (Join-Path $out 'libhost.a') -o (Join-Path $out 'test_mod.dll')
if ($LASTEXITCODE) { throw 'test_mod.dll did not build' }
foreach ($dll in 'libc++.dll', 'libunwind.dll') { Copy-Item (Join-Path $bin $dll) $out }
& (Join-Path $bin 'clang.exe') --target=x86_64-w64-mingw32 -O2 -shared (Join-Path $here 'test_tls.c') -o (Join-Path $out 'test_tls.dll')
if ($LASTEXITCODE) { throw 'test_tls.dll did not build' }

cl /nologo /O2 /W4 /MT /utf-8 /Fo"$out\\" (Join-Path $here 'test_loader.c') (Join-Path $here '..\pe_loader.c') /link /OUT:"$out\test_loader.exe"
if ($LASTEXITCODE) { throw 'test_loader.exe did not build' }

& (Join-Path $out 'test_loader.exe') (Join-Path $out 'test_mod.dll') (Join-Path $out 'test_tls.dll')
exit $LASTEXITCODE
