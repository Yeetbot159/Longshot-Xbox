# Build the launcher into xbox\out\Longshot.msix (and the certificate beside it), ready for Device Portal.
#     powershell -ExecutionPolicy Bypass -File xbox\app\build.ps1 [-Version 1.2.3.0] [-Mesa <folder>] [-Mingw <folder>] [-Renderer <longshot_vk.dll>]
#
# The package holds what is the same for every player and holds nothing of the game: the launcher, the TLS slot, Mesa's
# Dozen (vulkan_dzn.dll, dxil.dll: mesa-dist-win's x64 folder), llvm-mingw's C++ runtime (the game module's and the
# renderer's), and the renderer built for the Xbox (LONGSHOT_XBOX=1 RX_VK_ONLY=1, nx2/build.py), when one is given.
param(
    [string]$Version = '1.0.0.0',
    [string]$Mesa = $(if ($env:LONGSHOT_MESA) { $env:LONGSHOT_MESA } else { 'D:\longshot-tools\mesa-eval\x\x64' }),
    [string]$Mingw = $(if ($env:LONGSHOT_MINGW) { $env:LONGSHOT_MINGW } else { 'D:\longshot-tools\llvm-mingw-20261104-ucrt-x86_64' }),
    [string]$Renderer = '',
    [switch]$IncludeTools,
    [string]$PythonVenv = 'D:\longshot-tools\venv',
    [string]$PythonHome = 'C:\Program Files\Python314'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common.ps1')
Enter-VsEnv

$obj = Join-Path $XboxOut 'app-obj'
$pkg = Join-Path $XboxOut 'app-pkg'
Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $obj, $pkg | Out-Null

$sdkInc = Join-Path (Split-Path (Split-Path (Get-SdkBin))) '..\Include' | Resolve-Path
$ver = Split-Path (Split-Path (Get-SdkBin)) -Leaf
$cppwinrt = Join-Path $sdkInc "$ver\cppwinrt"
$src = 'launcher.cpp', 'platform.cpp', 'present.cpp', 'modules.cpp', 'dcomp_shim.cpp', 'profiles.cpp', 'game_files.cpp' |
    ForEach-Object { Join-Path $PSScriptRoot $_ }
$src += Join-Path $PSScriptRoot '..\loader\pe_loader.c'

cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /permissive- /utf-8 /DUNICODE /D_UNICODE /I $cppwinrt /Fo"$obj\\" $src `
    /link /SUBSYSTEM:WINDOWS /APPCONTAINER /OUT:"$pkg\Longshot.exe" `
    WindowsApp.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib xaudio2.lib
if ($LASTEXITCODE) { throw "Longshot.exe did not build ($LASTEXITCODE)" }

# (a real TLS index from Windows for the modules loaded by hand that have thread-locals: Dozen's, xbox/loader)
cl /nologo /O2 /MT /W4 /Fo"$obj\\" (Join-Path $PSScriptRoot 'tls_slot.c') /LD /link /APPCONTAINER /OUT:"$pkg\longshot_tls.dll" /IMPLIB:"$obj\longshot_tls.lib"
if ($LASTEXITCODE) { throw 'longshot_tls.dll did not build' }

if ($Renderer) {
    foreach ($dll in 'vulkan_dzn.dll', 'dxil.dll') {
        $p = Join-Path $Mesa $dll
        if (-not (Test-Path $p)) { throw "no $dll in $Mesa (mesa-dist-win's x64 folder: pass -Mesa or set LONGSHOT_MESA)" }
        Copy-Item $p $pkg
    }
}
foreach ($dll in 'libc++.dll', 'libunwind.dll') {
    $p = Join-Path $Mingw "x86_64-w64-mingw32\bin\$dll"
    if (-not (Test-Path $p)) { $p = Join-Path $Mingw "bin\$dll" }
    if (-not (Test-Path $p)) { throw "no $dll in $Mingw (pass -Mingw or set LONGSHOT_MINGW)" }
    Copy-Item $p $pkg
}
if ($Renderer) {
    if (-not (Test-Path $Renderer)) { throw "no renderer at $Renderer" }
    Copy-Item $Renderer (Join-Path $pkg 'longshot_vk.dll')
} else {
    Write-Warning 'no -Renderer: this is a launcher-only package; it cannot render the game'
}

if ($IncludeTools) {
    Write-Host 'Bundling build tools (Python, llvm-mingw, nx2/ scripts) into package...'
    $tools = Join-Path $pkg 'tools'
    New-Item -ItemType Directory -Force $tools | Out-Null

    # 1. Standalone Python
    $py = Join-Path $tools 'python'
    New-Item -ItemType Directory -Force $py | Out-Null
    Copy-Item (Join-Path $PythonHome 'python.exe') $py
    Copy-Item (Join-Path $PythonHome 'python3*.dll') $py
    Copy-Item (Join-Path $PythonHome 'vcruntime140*.dll') $py
    foreach ($directory in 'DLLs', 'Lib') {
        $source = Join-Path $PythonHome $directory
        $destination = Join-Path $py $directory
        New-Item -ItemType Directory -Force $destination | Out-Null
        Copy-Item -Recurse -Force (Join-Path $source '*') $destination
    }
    if (Test-Path (Join-Path $PythonVenv 'Lib\site-packages')) {
        $site = Join-Path $py 'Lib\site-packages'
        New-Item -ItemType Directory -Force $site | Out-Null
        Copy-Item -Recurse -Force (Join-Path $PythonVenv 'Lib\site-packages\*') $site
    }

    # 2. llvm-mingw
    $m = Join-Path $tools 'llvm-mingw'
    New-Item -ItemType Directory -Force $m | Out-Null
    foreach ($sub in 'bin', 'include', 'lib', 'x86_64-w64-mingw32') {
        $p = Join-Path $Mingw $sub
        if (Test-Path $p) { Copy-Item -Recurse $p (Join-Path $m $sub) }
    }

    # 3. Source scripts and runtime shim
    foreach ($dir in 'nx2', 'host', 'third_party') {
        $p = Join-Path $RepoRoot $dir
        if (Test-Path $p) { Copy-Item -Recurse $p (Join-Path $tools $dir) }
    }
    Write-Host 'Tool bundling completed.'
}

if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "a package version is four numbers (1.2.3.0), not '$Version'" }
(Get-Content (Join-Path $PSScriptRoot 'AppxManifest.xml') -Raw) -replace 'Version="1\.0\.0\.0"', "Version=`"$Version`"" |
    Set-Content -Encoding utf8 (Join-Path $pkg 'AppxManifest.xml')
python -I (Join-Path $XboxRoot 'make_assets.py') $pkg
if ($LASTEXITCODE) { throw 'make_assets.py failed' }

$msix = Join-Path $XboxOut 'Longshot.msix'
New-SignedPackage $pkg $msix
Write-Host "built $msix"
