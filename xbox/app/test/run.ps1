# Build and run the launcher's portable parts' test on a PC.
#     powershell -ExecutionPolicy Bypass -File xbox\app\test\run.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\common.ps1')
Enter-VsEnv
$out = Join-Path $XboxOut 'app-test'
New-Item -ItemType Directory -Force $out | Out-Null
$src = (Join-Path $PSScriptRoot 'test_app.cpp'), (Join-Path $PSScriptRoot '..\profiles.cpp'), (Join-Path $PSScriptRoot '..\game_files.cpp')
cl /nologo /std:c++20 /EHsc /O2 /W4 /MT /utf-8 /Fo"$out\\" $src /link /OUT:"$out\test_app.exe"
if ($LASTEXITCODE) { throw 'test_app.exe did not build' }
& (Join-Path $out 'test_app.exe')
exit $LASTEXITCODE
