# FluxDM

A multi-connection download manager in C++ / Qt 6, modeled on Free Download Manager 6
(same stack: MSVC + Qt Quick/QML + SQLite, with protocol "modules" behind an engine interface).

## Build

```powershell
scripts\setup-toolchain.ps1          # one time: VS Build Tools, CMake, Ninja, Python, Qt 6.8.3, vcpkg
scripts\build.ps1                    # configure + build + windeployqt  -> build\release\FluxDM.exe
scripts\build.ps1 -Test              # ... and run the test suite
scripts\build.ps1 -Run               # ... and launch the app
scripts\package.ps1                  # portable release zip -> dist\FluxDM-<version>-win64.zip (with ffmpeg + yt-dlp)
scripts\package.ps1 -Installer       # ... plus FluxDM-<version>-setup.exe + latest.json (needs Inno Setup 6)
scripts\package.ps1 -NoTools         # ... without ffmpeg/yt-dlp (FluxDM can download them itself)
scripts\build.ps1 -UpdateTranslations # refresh i18n\*.ts from the sources (see i18n\README.md)
scripts\shot.ps1                     # headless screenshots of every view -> build\shots
```

CLI: `FluxDM.exe <url> [<url>...]` opens the Add dialog; `FluxDM.exe --silent <url>` adds directly.
A second launch forwards its arguments to the running instance.

Manual testing: `python scripts\range_server.py <dir> --rate-kbps 256` serves files with Range support,
throttled per connection.

## Install on another PC

Copy `dist\FluxDM-<version>-setup.exe` to the other PC and run it. No administrator rights are needed
(it installs for the current user; "for all users" is offered too). Setup adds a Start-menu entry and an
uninstaller (Settings > Apps), and offers to open .torrent files and magnet links with FluxDM, to connect
it to your browsers and to start it with Windows. ffmpeg and yt-dlp come with it (`tools\`); FluxDM
checks for newer versions once a day and shows an "Update" button in Settings > Downloads only when one
exists. The setup is not code-signed, so Windows SmartScreen warns the first time: "More info" > "Run
anyway". Uninstalling keeps your download list and settings unless you choose to delete them.

Browser extension: after installing, load it from `<install folder>\browser\chromium` (Chrome, Edge,
Brave: Extensions > Developer mode > Load unpacked) or `browser\firefox` - see Settings > Browser.

The portable zip works too: unpack it anywhere and run `FluxDM.exe`.

Silent install for scripts: `FluxDM-<version>-setup.exe /VERYSILENT /CURRENTUSER /TASKS="assoc,browser"`.

## Layout

| Path | What |
|---|---|
| `src/core/` | `Download` (record + live stats), `DownloadManager` (queue, priorities, traffic modes; split over `DownloadManager*.cpp`), `Database` (FDM-shaped SQLite), `FileNamer`, `DownloadEngine` (module interface), `DiskSpace`, `Checksum`, `Metalink`, `ReleaseSignature` |
| `src/engines/www/` | module `downloadswww`: `CurlWorker` (one thread, one `curl_multi`), `HttpTask` (probe, dynamic segment splitting, resume, retries), `FileWriter` (sparse, positional writes) |
| `src/ui/` | `AppController` (QML singleton `App`, split over `AppController*.cpp`), models, tray icon |
| `qml/` | UI: top bar, tag filter chips, download list, bottom panel (General/Files/Log), status bar with Low/Medium/High traffic mode, dialogs |
| `src/tools/` | `fluxdm-sign`: release signing for the updater (not shipped) |
| `tests/` | Qt Test suite with an in-process Range HTTP server |

Data: `%LOCALAPPDATA%\FluxDM\FluxDM\` (`db.sqlite`, `settings.ini`, `backup\db\`).

## Status

Download modules (FDM's plugin layout):
- **downloadswww** – HTTP/HTTPS/FTP: up to 32 connections with dynamic splitting, crash-safe resume,
  retries, Content-Disposition names, system or manual (HTTP/SOCKS5) proxy, per-download, global and
  per-server limits, server logins
  (401/FTP; saved per server, DPAPI-encrypted), "file exists" choice (keep both / replace / ask), clear
  errors for dead links (web page instead of the file, expired signed links), mirrors, POST (form)
  downloads, trust-per-server for untrusted certificates, Metalink (`.meta4`/`.metalink`: mirrors +
  checksum), IPv4/IPv6 choice, own user agent.
- **downloadsbt** – BitTorrent (libtorrent 2.1): magnets, local/web `.torrent`, choose files before
  downloading, Peers/Trackers tabs, download in order, seeding to a share ratio, upload limit, DHT/LSD/UPnP,
  opt-in magnet/.torrent association, watch folder for `.torrent` files, listening port, "Create torrent
  and share" for finished files.
- **downloadsm3u** – HLS video streams: best variant, parallel segments, AES-128, fMP4, resume, MP4 remux
  with ffmpeg.
- **downloadsfragments** – DASH (`.mpd`) videos on demand: best video + audio, SegmentTemplate/Timeline/List/
  Base, resume, combined into one file with ffmpeg (stream copy).
- **downloadsms** – videos from YouTube, Vimeo, Twitch, SoundCloud and similar sites through yt-dlp (best
  video + audio, merged with ffmpeg), stop/continue.

App:
- Traffic modes Snail/Medium/High with a weekly **snail schedule**; per-download schedules;
  "when done" sleep/hibernate/shut down/exit; automatic retries; batch links `file[1-20].zip`.
- Browser integration: `fluxdm-nativehost.exe` + Chrome/Edge/Brave/Firefox extension (catches downloads,
  context menu, detects HLS/DASH streams on the page, site exclusions, Alt+click to skip, "download all
  links on this page").
- Library: user tags (colours, auto-assign by file type, per-tag folder), MP3/MP4 converter, play while
  downloading (torrents), multi-select, sorting (date/name/size/status), folders by file type, move to folder, rename,
  open/open with, missing-file detection, import/export, per-download speed limit, completion notification
  + sound, keeps the PC awake while downloading, tray, clipboard monitor, light/dark, manual queue order,
  separate download windows, zoom, import from Free Download Manager, duplicate-link warning, checksum
  check (MD5/SHA-1/SHA-256/SHA-512), extract finished archives (7-Zip or Windows tar), speed graph and
  traffic totals (today/month/all time), auto-remove finished entries, start with Windows, keyboard
  shortcuts (listed in About), screen-reader names and Tab focus on the custom controls.
- Robustness: waits for disk space and for a missing drive instead of failing; data is flushed to disk
  before progress is saved (on a helper thread); failed database writes are reported; DB writes are
  transactional.
- Safety: antivirus check + Mark of the Web for finished files, metered-connection handling; browser
  cookies, form data and the remote access code stored DPAPI-protected; header values from the browser
  sanitised; the single-instance channel accepts only the current Windows user.
- Remote control from the local network (web page, access code; off by default; Host-header check,
  idle timeouts, per-address and global sign-in limits, expiring tokens).
- App log, crash dumps and "Report a problem" (nothing is sent automatically); updater (SHA-256 checked,
  optionally Ed25519-signed manifests) and an Inno Setup installer; ffmpeg and yt-dlp shipped and
  kept up to date (GitHub releases, SHA-256 checked); translations (Spanish
  complete, machine-made).
- UI built from a small design system (`qml/F*.qml`), full-page settings.

## Signed updates

Once you have a release host, sign update manifests so a tampered `latest.json` can't make FluxDM run
another installer:

```powershell
build\release\fluxdm-sign.exe keygen D:\keys\fluxdm-release.key   # once; prints the public key
cmake --preset msvc-release -DFLUXDM_UPDATE_PUBLIC_KEY=<public key>  # apps built so require signatures
$env:FLUXDM_SIGNING_KEY = "D:\keys\fluxdm-release.key"; scripts\package.ps1 -Installer
```

Keep the private key out of the repository and off shared folders.

Not done yet: code signing (SmartScreen warns about the unsigned setup), a release host for updates and its signing key,
more languages, Linux/macOS builds, a separate folder for unfinished files, HLS/DASH disk writes off the
UI thread, and FDM 3.9's legacy features (site explorer, HTML spider, partial ZIP).