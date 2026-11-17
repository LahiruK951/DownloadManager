# Render FluxDM views to PNG without showing anything on screen.
# Uses a throw-away profile with simulated (demo://) downloads and Qt's offscreen platform.
#   scripts\shot.ps1                       # all views, light + dark
#   scripts\shot.ps1 -Views main,settings -Themes dark
param(
    [string[]]$Views = @("main", "panel", "panel-files", "panel-log", "add", "settings", "menu", "delete", "whendone"),
    [string[]]$Themes = @("light", "dark"),
    [string]$Preset = "msvc-release",
    [double]$Scale = 1.0
)
$ErrorActionPreference = "Stop"
# "-File" passes "a,b" as one string; accept both forms
$Views = @($Views | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Themes = @($Themes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$($Preset.Substring(5))"
$exe = Join-Path $bin "FluxDM.exe"
$out = Join-Path $root "build\shots"
New-Item -ItemType Directory -Force $out | Out-Null

# The offscreen platform plugin must be next to the exe, or Qt shows an error box.
$plugin = Join-Path $bin "platforms\qoffscreen.dll"
if (-not (Test-Path $plugin)) { throw "Missing $plugin (run scripts\build.ps1, which deploys it)" }

foreach ($theme in $Themes) {
    foreach ($view in $Views) {
        $profile = Join-Path $env:TEMP ("fluxdm-shot-" + [guid]::NewGuid().ToString("N"))
        $png = Join-Path $out "$view-$theme.png"
        Remove-Item $png -ErrorAction SilentlyContinue
        $env:FLUXDM_DATA_DIR = $profile
        $env:FLUXDM_DEV_DEMO = "1"
        $env:FLUXDM_THEME = $theme
        $env:FLUXDM_DEV_OPEN = if ($view -eq "main") { "" } else { $view }
        $env:FLUXDM_DEV_GRAB = $png
        $env:QT_QPA_PLATFORM = "offscreen"
        $env:QT_QUICK_BACKEND = "software"
        $env:QT_QPA_FONTDIR = "$env:WINDIR\Fonts"   # offscreen uses FreeType; point it at system fonts
        $env:QT_SCALE_FACTOR = "$Scale"
        $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden `
            -RedirectStandardError (Join-Path $out "$view-$theme.log") -RedirectStandardOutput "NUL"
        if (-not $p.WaitForExit(30000)) { $p.Kill(); Write-Warning "$view/$theme timed out" }
        Remove-Item -Recurse -Force $profile -ErrorAction SilentlyContinue
        $warn = Get-Content (Join-Path $out "$view-$theme.log") -ErrorAction SilentlyContinue |
            Where-Object { $_ -match "qml|QML|Error|error" -and $_ -notmatch "QSystemTrayIcon|tray" }
        $status = if (Test-Path $png) { "ok" } else { "MISSING" }
        Write-Host ("{0,-12} {1,-6} {2}" -f $view, $theme, $status)
        $warn | Select-Object -First 5 | ForEach-Object { Write-Host "    $_" }
    }
}
foreach ($v in "FLUXDM_DATA_DIR","FLUXDM_DEV_DEMO","FLUXDM_THEME","FLUXDM_DEV_OPEN","FLUXDM_DEV_GRAB","QT_QPA_PLATFORM","QT_QUICK_BACKEND","QT_SCALE_FACTOR","QT_QPA_FONTDIR") {
    Remove-Item "env:$v" -ErrorAction SilentlyContinue
}
Write-Host "Screenshots in $out"
