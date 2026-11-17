# FluxDM toolchain bootstrap: MSVC Build Tools, CMake, Ninja, Python, Qt 6 (aqtinstall), vcpkg.
# Safe to re-run; each step is skipped when already present.
param(
    [string]$QtVersion = "6.8.3",
    [string]$QtRoot    = "C:\Qt",
    [string]$VcpkgRoot = "C:\vcpkg"
)
$ErrorActionPreference = "Continue"

function Install-Winget($id, $extra) {
    $listed = winget list --id $id -e --accept-source-agreements 2>$null | Out-String
    if ($listed -match [regex]::Escape($id)) { Write-Host "[ok] $id already installed"; return }
    Write-Host "[..] installing $id"
    if ($extra) {
        winget install --id $id -e --accept-package-agreements --accept-source-agreements --override $extra
    } else {
        winget install --id $id -e --accept-package-agreements --accept-source-agreements
    }
}

# 1. Compilers & build tools
Install-Winget "Microsoft.VisualStudio.2022.BuildTools" "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
Install-Winget "Kitware.CMake"
Install-Winget "Ninja-build.Ninja"
Install-Winget "Python.Python.3.12"

# Refresh PATH for this session
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")

# 2. Qt via aqtinstall (no Qt account needed)
$qtDir = Join-Path $QtRoot "$QtVersion\msvc2022_64"
if (Test-Path "$qtDir\bin\qmake.exe") {
    Write-Host "[ok] Qt $QtVersion present"
} else {
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $py -or $py -like "*WindowsApps*") { $py = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
    & $py -m pip install --upgrade aqtinstall
    & $py -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -O $QtRoot
}

# 3. vcpkg
if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
    if (-not (Test-Path $VcpkgRoot)) { git clone https://github.com/microsoft/vcpkg $VcpkgRoot }
    & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
}
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "User")
[Environment]::SetEnvironmentVariable("FLUXDM_QT_DIR", $qtDir, "User")
Write-Host "[done] toolchain ready. VCPKG_ROOT=$VcpkgRoot  Qt=$qtDir"
