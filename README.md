# FluxDM

A multi-connection download manager in C++ / Qt 6, modeled on Free Download Manager 6
(same stack: MSVC + Qt Quick/QML + SQLite, with protocol "modules" behind an engine interface).

## Build

```powershell
scripts\setup-toolchain.ps1          # one time: VS Build Tools, CMake, Ninja, Python, Qt 6.8.3, vcpkg
scripts\build.ps1                    # configure + build + windeployqt  -> build\release\FluxDM.exe
scripts\build.ps1 -Test              # ... and run the test suite
scripts\build.ps1 -Run               # ... and launch the app
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

Done:
- **HTTP/HTTPS/FTP** (`downloadswww`): up to 32 connections per download with dynamic splitting,
  crash-safe resume, per-connection retries with backoff, Content-Disposition names, system proxy.
- **BitTorrent** (`downloadsbt`, libtorrent 2.1): magnet links, local and http `.torrent` files,
  per-file selection, DHT/LSD/UPnP, resume, seeding until a share ratio, upload limit,
  magnet/.torrent association (opt-in, HKCU).
- **Traffic modes** Snail (Low) / Medium / High with per-mode limits, plus a **weekly schedule**
  ("snail mode on weekdays 09:00–18:00"); picking a mode by hand overrides the active rule until it ends.
- **Per-download schedule** (FDM's `downloadsScheduleRecords`), **when-done action**
  (sleep / hibernate / shut down / exit, with a 30 s cancellable countdown).
- **Batch links** `file[1-20].zip`, `img[001-120].jpg`, `[a-f].txt`.
- Queue + priorities, tray, clipboard monitoring, drag & drop, single instance + CLI, light/dark theme.

Next: browser extension + native messaging host, HLS/DASH + yt-dlp/ffmpeg, tags with default folders,
auto-retry rules, import/export, remote control, installer.
