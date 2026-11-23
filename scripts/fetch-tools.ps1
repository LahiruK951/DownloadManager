# Put ffmpeg and yt-dlp into a release folder's tools\ (called by package.ps1).
#   scripts\fetch-tools.ps1 -Dest dist\FluxDM-0.1.0\tools
# Takes the latest releases from GitHub: yt-dlp (yt-dlp/yt-dlp) and gyan.dev's ffmpeg "essentials"
# build (GyanD/codexffmpeg; only ffmpeg.exe is kept — FluxDM and yt-dlp's merging need nothing else).
# Every file is checked against the SHA-256 GitHub publishes for it. Downloads are kept in
# third_party\tools\cache, so later packages work offline until a new release comes out.
# Writes tools\versions.json, which FluxDM reads to know what it ships (and to offer updates).
param([Parameter(Mandatory = $true)][string]$Dest)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is many times slower with the progress bar
$root = Split-Path -Parent $PSScriptRoot
$cache = Join-Path $root "third_party\tools\cache"
New-Item -ItemType Directory -Force $cache, $Dest | Out-Null
$known = Join-Path $cache "releases.json"   # last release info, for offline packaging

function Get-Release([string]$repo, [scriptblock]$pick) {
    $rel = Invoke-RestMethod -Headers @{ "User-Agent" = "FluxDM-packager"; "Accept" = "application/vnd.github+json" } `
        -Uri "https://api.github.com/repos/$repo/releases/latest" -TimeoutSec 60
    $asset = $rel.assets | Where-Object { & $pick $_.name } | Select-Object -First 1
    if (-not $asset) { throw "No matching download in the latest release of $repo" }
    if ($asset.digest -notmatch '^sha256:([0-9a-f]{64})$') { throw "$repo $($asset.name) has no SHA-256 digest" }
    [pscustomobject]@{ version = $rel.tag_name; name = $asset.name; url = $asset.browser_download_url; sha256 = $Matches[1] }
}

function Get-Cached($rel) {
    $file = Join-Path $cache $rel.name
    if ((Test-Path $file) -and (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower() -eq $rel.sha256) { return $file }
    Write-Host "Downloading $($rel.name) ($($rel.version))..."
    Invoke-WebRequest -Uri $rel.url -OutFile "$file.part" -UseBasicParsing -TimeoutSec 600
    if ((Get-FileHash -Algorithm SHA256 "$file.part").Hash.ToLower() -ne $rel.sha256) {
        Remove-Item "$file.part"
        throw "$($rel.name): SHA-256 does not match the release"
    }
    Move-Item -Force "$file.part" $file
    $file
}

try {
    $releases = [ordered]@{
        "yt-dlp" = Get-Release "yt-dlp/yt-dlp" { param($n) $n -eq "yt-dlp.exe" }
        "ffmpeg" = Get-Release "GyanD/codexffmpeg" { param($n) $n -match '^ffmpeg-[\w.\-]*essentials_build\.7z$' }
    }
    $releases | ConvertTo-Json | Set-Content -Encoding utf8 $known
} catch {
    if (-not (Test-Path $known)) { throw }
    Write-Warning "GitHub not reachable ($($_.Exception.Message)); using the last known releases"
    $j = Get-Content -Raw $known | ConvertFrom-Json
    $releases = [ordered]@{ "yt-dlp" = $j."yt-dlp"; "ffmpeg" = $j.ffmpeg }
}

# yt-dlp: a single program
Copy-Item -Force (Get-Cached $releases["yt-dlp"]) (Join-Path $Dest "yt-dlp.exe")

# ffmpeg: bin\ffmpeg.exe and the licence out of the archive (Windows' own tar reads 7z)
$archive = Get-Cached $releases["ffmpeg"]
$tmp = Join-Path $env:TEMP ("fluxdm-ffmpeg-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $tmp | Out-Null
try {
    & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $tmp "*/bin/ffmpeg.exe" "*/LICENSE"
    if ($LASTEXITCODE) { throw "tar could not unpack $archive" }
    $exe = Get-ChildItem $tmp -Recurse -Filter ffmpeg.exe | Select-Object -First 1
    $lic = Get-ChildItem $tmp -Recurse -Filter LICENSE | Select-Object -First 1
    if (-not $exe) { throw "ffmpeg.exe not found in $archive" }
    Copy-Item -Force $exe.FullName (Join-Path $Dest "ffmpeg.exe")
    if ($lic) { Copy-Item -Force $lic.FullName (Join-Path $Dest "ffmpeg-LICENSE.txt") }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

[ordered]@{ "yt-dlp" = $releases["yt-dlp"].version; "ffmpeg" = $releases["ffmpeg"].version } | ConvertTo-Json |
    Set-Content -Encoding ascii (Join-Path $Dest "versions.json")

Set-Content -Encoding ascii (Join-Path $Dest "README.txt") @"
Programs FluxDM uses, as published by their projects (FluxDM can update them: Settings > Downloads).

ffmpeg $($releases["ffmpeg"].version) - https://ffmpeg.org
  Windows build by gyan.dev (https://www.gyan.dev/ffmpeg/builds/), GPL v3: see ffmpeg-LICENSE.txt.
  Source code: https://ffmpeg.org/download.html and https://github.com/GyanD/codexffmpeg

yt-dlp $($releases["yt-dlp"].version) - https://github.com/yt-dlp/yt-dlp
  Released into the public domain (The Unlicense).
"@
Write-Host "Tools: yt-dlp $($releases["yt-dlp"].version), ffmpeg $($releases["ffmpeg"].version) -> $Dest"
