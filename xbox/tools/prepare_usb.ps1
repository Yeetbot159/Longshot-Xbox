# Prepare a USB drive for Longshot Xbox Developer Mode.
# Copies standalone Python, llvm-mingw, and nx2/ scripts into <Drive>:\Longshot\tools\
# Usage:
#     powershell -ExecutionPolicy Bypass -File xbox\tools\prepare_usb.ps1 -Drive E:
param(
    [Parameter(Mandatory=$true)]
    [string]$Drive,
    [string]$Mingw = $(if ($env:LONGSHOT_MINGW) { $env:LONGSHOT_MINGW } else { 'D:\longshot-tools\llvm-mingw-20261104-ucrt-x86_64' }),
    [string]$PythonVenv = 'D:\longshot-tools\venv',
    [string]$PythonHome = 'C:\Program Files\Python314'
)
$ErrorActionPreference = 'Stop'
$Drive = $Drive.TrimEnd('\')
if (-not (Test-Path "$Drive\")) { throw "Drive $Drive does not exist." }

$targetTools = Join-Path $Drive 'Longshot\tools'
Write-Host "Setting up Longshot build tools at $targetTools ..."
New-Item -ItemType Directory -Force $targetTools | Out-Null

# 1. Standalone Python runtime
$py = Join-Path $targetTools 'python'
New-Item -ItemType Directory -Force $py | Out-Null
Copy-Item (Join-Path $PythonHome 'python.exe') $py
Copy-Item (Join-Path $PythonHome 'python3*.dll') $py
Copy-Item (Join-Path $PythonHome 'vcruntime140*.dll') $py
Copy-Item -Recurse (Join-Path $PythonHome 'DLLs') $py
Copy-Item -Recurse (Join-Path $PythonHome 'Lib') $py
if (Test-Path (Join-Path $PythonVenv 'Lib\site-packages')) {
    Copy-Item -Recurse -Force (Join-Path $PythonVenv 'Lib\site-packages\*') (Join-Path $py 'Lib\site-packages')
}

# 2. llvm-mingw
$m = Join-Path $targetTools 'llvm-mingw'
New-Item -ItemType Directory -Force $m | Out-Null
foreach ($sub in 'bin', 'include', 'lib', 'x86_64-w64-mingw32') {
    $p = Join-Path $Mingw $sub
    if (Test-Path $p) { Copy-Item -Recurse $p (Join-Path $m $sub) }
}

# 3. Source scripts
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
foreach ($dir in 'nx2', 'host', 'third_party') {
    $p = Join-Path $repoRoot $dir
    if (Test-Path $p) { Copy-Item -Recurse $p (Join-Path $targetTools $dir) }
}

Write-Host "USB drive successfully prepared at $targetTools"
