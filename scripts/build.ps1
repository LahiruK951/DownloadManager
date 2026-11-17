# Configure + build FluxDM with MSVC/Ninja, deploy Qt runtime, optionally run tests or the app.
param(
    [ValidateSet("msvc-release", "msvc-debug")][string]$Preset = "msvc-release",
    [switch]$ConfigureOnly,
    [switch]$Test,
    [switch]$Run
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# PATH / env from the registry (fresh installs aren't visible to old shells)
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $env:VCPKG_ROOT)    { $env:VCPKG_ROOT    = [Environment]::GetEnvironmentVariable("VCPKG_ROOT", "User") }
if (-not $env:FLUXDM_QT_DIR) { $env:FLUXDM_QT_DIR = [Environment]::GetEnvironmentVariable("FLUXDM_QT_DIR", "User") }
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $n = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($n) { $env:Path = "$($n.DirectoryName);$env:Path" }
}

# MSVC developer environment
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "MSVC Build Tools not found; run scripts\setup-toolchain.ps1" }
    $env:Path = "$(Split-Path $vswhere);$env:Path"
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

Push-Location $root
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed" }
    if ($ConfigureOnly) { return }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed" }

    $bin = Join-Path $root "build\$($Preset.Substring(5))"
    $exe = Join-Path $bin "FluxDM.exe"
    $deployFlag = if ($Preset -eq "msvc-debug") { "--debug" } else { "--release" }
    & "$env:FLUXDM_QT_DIR\bin\windeployqt.exe" $deployFlag --qmldir "$root\qml" --no-translations --no-system-d3d-compiler --no-opengl-sw $exe | Out-Null
    if ($LASTEXITCODE) { throw "windeployqt failed" }
    Write-Host "Built: $exe"

    if ($Test) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "tests failed" }
    }
    if ($Run) { Start-Process $exe }
}
finally { Pop-Location }
