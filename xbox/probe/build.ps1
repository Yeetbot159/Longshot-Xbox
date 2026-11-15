# Build the probe into xbox\out\LongshotXboxProbe.msix (and the certificate beside it), ready for Device Portal.
#     powershell -ExecutionPolicy Bypass -File xbox\probe\build.ps1
. (Join-Path $PSScriptRoot '..\common.ps1')
Enter-VsEnv

$obj = Join-Path $XboxOut 'probe-obj'
$pkg = Join-Path $XboxOut 'probe-pkg'
Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $obj, $pkg | Out-Null

$sdkInc = Join-Path (Split-Path (Split-Path (Get-SdkBin))) '..\Include' | Resolve-Path
$ver = Split-Path (Split-Path (Get-SdkBin)) -Leaf
$cppwinrt = Join-Path $sdkInc "$ver\cppwinrt"

cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /permissive- /utf-8 /DUNICODE /D_UNICODE /I $cppwinrt `
    /Fo"$obj\\" (Join-Path $PSScriptRoot 'probe.cpp') `
    /link /SUBSYSTEM:WINDOWS /APPCONTAINER /OUT:"$pkg\probe.exe" `
    WindowsApp.lib d3d12.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib
if ($LASTEXITCODE) { throw "probe.exe did not build ($LASTEXITCODE)" }

Copy-Item (Join-Path $PSScriptRoot 'AppxManifest.xml') $pkg
foreach ($dll in 'dxcompiler.dll', 'dxil.dll') { Copy-Item (Join-Path (Get-SdkBin) $dll) $pkg }
python -I (Join-Path $XboxRoot 'make_assets.py') $pkg
if ($LASTEXITCODE) { throw 'make_assets.py failed' }

$msix = Join-Path $XboxOut 'LongshotXboxProbe.msix'
New-SignedPackage $pkg $msix
Write-Host "built $msix"
Write-Host "certificate $(Join-Path $XboxOut 'LongshotXbox.cer')"
