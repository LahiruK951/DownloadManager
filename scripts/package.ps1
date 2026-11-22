# Build a portable release: dist\FluxDM-<version>-win64.zip
#   scripts\package.ps1            (builds msvc-release first)
#   scripts\package.ps1 -NoBuild
#   scripts\package.ps1 -Installer [-UpdateBaseUrl https://example.org/fluxdm]
#       also dist\FluxDM-<version>-setup.exe (Inno Setup 6) and dist\latest.json for the updater
param([switch]$NoBuild, [switch]$Installer, [string]$UpdateBaseUrl = "")
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

if ($Installer) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
              "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }
    if (-not $iscc) {
        throw "Inno Setup 6 not found. Install it (winget install JRSoftware.InnoSetup, or https://jrsoftware.org/isdl.php) and run again."
    }
    $dist = Join-Path $root "dist"
    & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DOutputDir=$dist" (Join-Path $root "installer\FluxDM.iss")
    if ($LASTEXITCODE) { throw "ISCC failed ($LASTEXITCODE)" }
    $setup = Join-Path $dist "FluxDM-$version-setup.exe"
    $sha = (Get-FileHash -Algorithm SHA256 $setup).Hash.ToLower()
    # The update manifest FluxDM's "Check for updates" reads (publish it next to the installer).
    $url = if ($UpdateBaseUrl) { $UpdateBaseUrl.TrimEnd('/') + "/FluxDM-$version-setup.exe" } else { "FluxDM-$version-setup.exe" }
    # Signed with the release key when $env:FLUXDM_SIGNING_KEY names its file (fluxdm-sign keygen);
    # apps built with -DFLUXDM_UPDATE_PUBLIC_KEY refuse unsigned manifests.
    $signature = ""
    if ($env:FLUXDM_SIGNING_KEY) {
        $signer = Join-Path $root "build\release\fluxdm-sign.exe"
        $signature = (& $signer sign $env:FLUXDM_SIGNING_KEY $version $sha).Trim()
        if ($LASTEXITCODE) { throw "signing failed" }
    }
    [ordered]@{ version = $version; url = $url; sha256 = $sha; signature = $signature; notes = "" } | ConvertTo-Json |
        Set-Content -Encoding utf8 (Join-Path $dist "latest.json")
    Write-Host "Installer $setup (sha256 $sha), update manifest dist\latest.json"
}
