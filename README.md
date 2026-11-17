# FluxDM

A multi-connection download manager in C++ / Qt 6, modeled on Free Download Manager 6
(same stack: MSVC + Qt Quick/QML + SQLite, with protocol "modules" behind an engine interface).

## Build

```powershell
scripts\setup-toolchain.ps1          # one time: VS Build Tools, CMake, Ninja, Python, Qt 6.8.3, vcpkg
scripts\build.ps1                    # configure + build + windeployqt  -> build\release\FluxDM.exe
scripts\build.ps1 -Test              # ... and run the test suite
scripts\build.ps1 -Run               # ... and launch the app
scripts\package.ps1                  # portable release zip -> dist\FluxDM-<version>-win64.zip
scripts\shot.ps1                     # headless screenshots of every view -> build\shots
```

CLI: `FluxDM.exe <url> [<url>...]` opens the Add dialog; `FluxDM.exe --silent <url>` adds directly.
A second launch forwards its arguments to the running instance.

Manual testing: `python scripts\range_server.py <dir> --rate-kbps 256` serves files with Range support,
throttled per connection.

## Layout

| Path | What |
|---|---|
| `src/core/` | `Download` (record + live stats), `DownloadManager` (queue, priorities, traffic modes), `Database` (FDM-shaped SQLite), `FileNamer`, `DownloadEngine` (module interface) |
| `src/engines/www/` | module `downloadswww`: `CurlWorker` (one thread, one `curl_multi`), `HttpTask` (probe, dynamic segment splitting, resume, retries), `FileWriter` (sparse, positional writes) |
| `src/ui/` | `AppController` (QML singleton `App`), models, tray icon |
| `qml/` | UI: top bar, tag filter chips, download list, bottom panel (General/Files/Log), status bar with Low/Medium/High traffic mode, dialogs |
| `tests/` | Qt Test suite with an in-process Range HTTP server |

Data: `%LOCALAPPDATA%\FluxDM\FluxDM\` (`db.sqlite`, `settings.ini`, `backup\db\`).

## Status

Download modules (FDM's plugin layout):
- **downloadswww** – HTTP/HTTPS/FTP: up to 32 connections with dynamic splitting, crash-safe resume,
  retries, Content-Disposition names, system proxy, per-download and global speed caps.
- **downloadsbt** – BitTorrent (libtorrent 2.1): magnets, local/web `.torrent`, file selection, seeding to a
  share ratio, upload limit, DHT/LSD/UPnP, opt-in magnet/.torrent association.
- **downloadsm3u** – HLS video streams: best variant, parallel segments, AES-128, fMP4, resume.

App:
- Traffic modes Snail/Medium/High with a weekly **snail schedule**; per-download schedules;
  "when done" sleep/hibernate/shut down/exit; automatic retries; batch links `file[1-20].zip`.
- Browser integration: `fluxdm-nativehost.exe` + Chrome/Edge/Brave extension (catches downloads,
  context menu, detects video streams on the page).
- Library: multi-select, folders by file type, move to folder, missing-file detection, import/export,
  per-download speed limit, completion notification + sound, tray, clipboard monitor, light/dark.
- UI built from a small design system (`qml/F*.qml`), full-page settings.

Next: ffmpeg conversion of streams to MP4, DASH, site extractors, remote control, installer.