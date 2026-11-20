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
scripts\package.ps1 -Installer       # ... plus FluxDM-<version>-setup.exe + latest.json (needs Inno Setup 6)
scriptsuild.ps1 -UpdateTranslations # refresh i18n\*.ts from the sources (see i18n\README.md)
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
  retries, Content-Disposition names, system or manual (HTTP/SOCKS5) proxy, per-download, global and
  per-server limits, server logins
  (401/FTP; saved per server, DPAPI-encrypted), "file exists" choice (keep both / replace / ask), clear
  errors for dead links (web page instead of the file, expired signed links), mirrors, POST (form)
  downloads, trust-per-server for untrusted certificates.
- **downloadsbt** – BitTorrent (libtorrent 2.1): magnets, local/web `.torrent`, choose files before
  downloading, Peers/Trackers tabs, download in order, seeding to a share ratio, upload limit, DHT/LSD/UPnP,
  opt-in magnet/.torrent association, watch folder for `.torrent` files.
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
  context menu, detects HLS/DASH streams on the page).
- Library: user tags (colours, auto-assign by file type, per-tag folder), MP3/MP4 converter, play while
  downloading (torrents), multi-select, sorting (date/name/size/status), folders by file type, move to folder, rename,
  open/open with, missing-file detection, import/export, per-download speed limit, completion notification
  + sound, keeps the PC awake while downloading, tray, clipboard monitor, light/dark, manual queue order,
  separate download windows, zoom, import from Free Download Manager.
- Safety: antivirus check + Mark of the Web for finished files, metered-connection handling.
- Remote control from the local network (web page, access code; off by default).
- App log, crash dumps and "Report a problem" (nothing is sent automatically); updater (SHA-256 checked)
  and an Inno Setup installer script; translations (Spanish complete, machine-made).
- UI built from a small design system (`qml/F*.qml`), full-page settings.

Not done yet: building the installer (needs Inno Setup), a release host for updates, more languages,
Linux/macOS builds, and FDM 3.9's legacy features (site explorer, HTML spider, partial ZIP).