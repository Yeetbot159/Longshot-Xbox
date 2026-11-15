# Shared by the Xbox build scripts (dot-source it): Visual Studio's x64 build environment, the Windows SDK's tools,
# and packing + signing a folder into an .msix that Device Portal installs.

$ErrorActionPreference = 'Stop'
$XboxRoot = $PSScriptRoot
$RepoRoot = Split-Path $XboxRoot -Parent
$XboxOut = Join-Path $XboxRoot 'out'
$CertSubject = 'CN=LongshotXbox'

function Enter-VsEnv {
    if ($env:VCToolsInstallDir) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the x64 C++ tools is not installed (Build Tools are enough)' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

function Get-SdkBin {
    $root = 'C:\Program Files (x86)\Windows Kits\10\bin'
    $ver = Get-ChildItem $root -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'x64\makeappx.exe') } |
        Sort-Object { [version]$_.Name } | Select-Object -Last 1
    if (-not $ver) { throw 'Windows SDK with makeappx.exe not found' }
    Join-Path $ver.FullName 'x64'
}

function Get-SigningCert {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } |
        Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type Custom -Subject $CertSubject -KeyUsage DigitalSignature `
            -FriendlyName 'Longshot Xbox (sideloading)' -CertStoreLocation Cert:\CurrentUser\My `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') -NotAfter (Get-Date).AddYears(5)
    }
    New-Item -ItemType Directory -Force $XboxOut | Out-Null
    Export-Certificate -Cert $cert -FilePath (Join-Path $XboxOut 'LongshotXbox.cer') | Out-Null
    $cert
}

function New-SignedPackage([string]$Folder, [string]$Msix) {
    $bin = Get-SdkBin
    & (Join-Path $bin 'makeappx.exe') pack /o /d $Folder /p $Msix | Out-Null
    if ($LASTEXITCODE) { throw "makeappx failed ($LASTEXITCODE)" }
    $cert = Get-SigningCert
    & (Join-Path $bin 'signtool.exe') sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My $Msix
    if ($LASTEXITCODE) { throw "signtool failed ($LASTEXITCODE)" }
}
