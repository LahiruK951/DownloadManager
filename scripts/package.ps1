# Build a portable release: dist\FluxDM-<version>-win64.zip
#   scripts\package.ps1            (builds msvc-release first)
#   scripts\package.ps1 -NoBuild
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\release"

if (-not $NoBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1") -Preset msvc-release
    if ($LASTEXITCODE) { throw "build failed" }
}
if (-not $env:FLUXDM_QT_DIR) { $env:FLUXDM_QT_DIR = [Environment]::GetEnvironmentVariable("FLUXDM_QT_DIR", "User") }

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(FluxDM VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$stage = Join-Path $root "dist\FluxDM-$version"
$zip = Join-Path $root "dist\FluxDM-$version-win64.zip"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

# Our executables + every DLL vcpkg placed next to them (curl, libtorrent, boost, openssl, ...)
Copy-Item (Join-Path $bin "FluxDM.exe"), (Join-Path $bin "fluxdm-nativehost.exe") $stage
Get-ChildItem $bin -Filter *.dll | Where-Object { $_.Name -notmatch '^Qt6' } | Copy-Item -Destination $stage
Copy-Item -Recurse (Join-Path $bin "browser") (Join-Path $stage "browser")

# Qt runtime for both executables
& "$env:FLUXDM_QT_DIR\bin\windeployqt.exe" --release --qmldir (Join-Path $root "qml") --no-translations `
    --no-system-d3d-compiler --no-opengl-sw --dir $stage (Join-Path $stage "FluxDM.exe") | Out-Null
if ($LASTEXITCODE) { throw "windeployqt failed" }
& "$env:FLUXDM_QT_DIR\bin\windeployqt.exe" --release --no-translations --dir $stage (Join-Path $stage "fluxdm-nativehost.exe") | Out-Null

# MSVC runtime, deployed app-locally so no VC++ redistributable install is needed
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $crt) { throw "MSVC redistributable CRT folder not found under $vs" }
Copy-Item (Join-Path $crt "*.dll") $stage

Set-Content -Path (Join-Path $stage "README.txt") -Encoding ascii -Value @"
FluxDM $version - portable

Run FluxDM.exe. Settings and the download list are stored in
%LOCALAPPDATA%\FluxDM\FluxDM.

Browser integration: Settings > Browser (load the extension from the
browser\chromium folder - or browser\firefox for Firefox - then switch on
"Connect FluxDM to your browsers").

The MP3/MP4 converter and DASH streams need ffmpeg: put ffmpeg.exe next
to FluxDM.exe, on PATH, or set it in Settings > Downloads.
"@

Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
$size = [Math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "Packaged $zip ($size MB)"
