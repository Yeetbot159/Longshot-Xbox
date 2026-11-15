# Build and run test_dozen.cpp: Mesa's Dozen (Vulkan on Direct3D 12) loaded through the app's module set on a PC.
# Needs a Direct3D 12 GPU, the Vulkan SDK's headers and glslangValidator, and Mesa's Windows build (mesa-dist-win's
# x64 folder: vulkan_dzn.dll, dxil.dll).
#     powershell -ExecutionPolicy Bypass -File xbox\app\test\dozen.ps1 [-Mesa <folder>] [-VulkanSdk <folder>]
param(
    [string]$Mesa = $(if ($env:LONGSHOT_MESA) { $env:LONGSHOT_MESA } else { 'D:\longshot-tools\mesa-eval\x\x64' }),
    [string]$VulkanSdk = $(if ($env:VULKAN_SDK) { $env:VULKAN_SDK } else {
        (Get-ChildItem C:\VulkanSDK -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1).FullName })
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\common.ps1')
Enter-VsEnv

$here = $PSScriptRoot
$out = Join-Path $XboxOut 'dozen-test'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($dll in 'vulkan_dzn.dll', 'dxil.dll') {
    if (-not (Test-Path (Join-Path $Mesa $dll))) { throw "no $dll in $Mesa (pass -Mesa or set LONGSHOT_MESA)" }
    Copy-Item (Join-Path $Mesa $dll) $out
}
if (-not $VulkanSdk -or -not (Test-Path (Join-Path $VulkanSdk 'Include\vulkan\vulkan.h'))) { throw 'no Vulkan SDK (pass -VulkanSdk or set VULKAN_SDK)' }

$ErrorActionPreference = 'Continue'   # (glslang names the file on stderr, which Windows PowerShell would throw on)
& (Join-Path $VulkanSdk 'Bin\glslangValidator.exe') -V --target-env vulkan1.3 --vn kFillSpv -o (Join-Path $out 'fill_spv.h') (Join-Path $here 'fill.comp') 2>&1 | Out-Null
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE) { throw 'fill.comp did not compile' }

cl /nologo /O2 /MT /W4 /Fo"$out\\" (Join-Path $here '..\tls_slot.c') /LD /link /OUT:"$out\longshot_tls.dll" /IMPLIB:"$out\longshot_tls.lib"
if ($LASTEXITCODE) { throw 'longshot_tls.dll did not build' }

cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /I"$out" /I"$(Join-Path $VulkanSdk 'Include')" /Fo"$out\\" `
    (Join-Path $here 'test_dozen.cpp') (Join-Path $here '..\modules.cpp') (Join-Path $here '..\dcomp_shim.cpp') (Join-Path $here '..\..\loader\pe_loader.c') `
    /link /OUT:"$out\test_dozen.exe"
if ($LASTEXITCODE) { throw 'test_dozen.exe did not build' }

& (Join-Path $out 'test_dozen.exe') $out
exit $LASTEXITCODE
