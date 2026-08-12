# SteamOS / Legion Go smoke-test checklist

Run this checklist on a real Legion Go Z1 Extreme after major download/player/AppImage changes.

## AppImage startup

- [ ] AppImage starts from SteamOS Desktop Mode.
- [ ] AppImage starts when added as a non-Steam game in Gaming Mode.
- [ ] Chinese/Japanese/Korean text renders correctly.
- [ ] Login persists across restart.
- [ ] X11 session works.
- [ ] Wayland/gamescope session works.
- [ ] VA-API/hardware decoding works for a normal video.
- [ ] Bundled `ffmpeg` is found by the downloader.

## Handheld UI

- [ ] Fresh Legion Go + SteamOS configuration selects Handheld automatically.
- [ ] Existing user-selected UI profile is not overwritten.
- [ ] 16:10 layout has no clipped controls at 1280x800.
- [ ] UI remains usable at 1920x1200 and 2560x1600 output.
- [ ] External 1080p/4K display remains usable.
- [ ] Gamepad focus is obvious on download cards and settings cells.
- [ ] Touch input still works.
- [ ] Home-screen Y opens Download Manager.
- [ ] Video-detail Y opens the download selector.
- [ ] Existing LB/RB/LT/RT navigation still works.

## Single-video download

- [ ] Quality list appears and includes available account qualities.
- [ ] Size estimate is shown when bandwidth/duration are available.
- [ ] AVC / HEVC / AV1 choices appear when the API exposes them.
- [ ] Audio choices include standard, Dolby, or Hi-Res tracks when available.
- [ ] Free-space check blocks a task when storage is insufficient.
- [ ] Download directory defaults to `~/Videos/wiliwili`.
- [ ] Changing download directory affects new tasks.
- [ ] Completed DASH video is muxed into `video.mp4` when ffmpeg succeeds.
- [ ] Raw DASH tracks remain playable when muxing is unavailable/fails.

## Batch download

- [ ] Multi-P video offers Current / All.
- [ ] UGC season/collection offers Current / All.
- [ ] PGC season offers Current / All.
- [ ] Existing completed/queued episodes are skipped in All.
- [ ] Each sibling refreshes its own source URL before transfer.
- [ ] Batch folder names are stable and collisions are disambiguated.

## Resume/recovery

- [ ] Pause during video track and resume continues from partial data.
- [ ] Pause after video track while audio is downloading does not redownload video.
- [ ] Kill the application during transfer; restart restores the task paused.
- [ ] Resume after restart continues safely.
- [ ] Disable Wi-Fi during transfer; reconnect and retry resumes with refreshed CDN URL.
- [ ] Suspend SteamOS during transfer; wake and task recovers/retries without corrupting media.
- [ ] CDN ignoring HTTP Range never causes duplicated bytes.

## Offline extras / metadata

- [ ] `source.json` contains durable BVID/AID/CID/quality/codec metadata.
- [ ] Temporary signed URLs are in `debug_source.json`, not the durable source fields.
- [ ] `cover.jpg` downloads when enabled.
- [ ] `danmaku.xml` downloads when enabled.
- [ ] Available subtitles produce SRT and ASS when enabled.
- [ ] `info.json` and `README.txt` are present.

## Download Manager / Offline Library

- [ ] Stage text follows source -> space -> video -> audio -> mux -> extras -> complete.
- [ ] Pause / Resume / Retry / Cancel work.
- [ ] Move Up / Move Down changes pending queue order.
- [ ] Configured concurrency 1-4 is respected.
- [ ] Normal download speed limit is respected approximately.
- [ ] Playback-specific speed limit takes effect while streaming a video.
- [ ] Open Folder opens the task folder in Desktop Mode (or falls back to showing the path).
- [ ] Original source page opens.
- [ ] Completed item appears in Offline Library.
- [ ] Offline item plays with network disabled.
- [ ] Separate DASH audio and subtitle are attached by the offline player when needed.
- [ ] Delete removes both the task and downloaded directory after confirmation.

## AppImage CI / release

- [ ] Pull request builds an AppImage artifact.
- [ ] `yoga` push builds an AppImage artifact.
- [ ] `.sha256` matches the AppImage.
- [ ] `.zsync` is generated.
- [ ] Extracted AppImage contains `usr/bin/wiliwili` and `usr/bin/ffmpeg`.
- [ ] `ldd` check has no `not found` entries.
- [ ] `v*` tag creates/updates a GitHub Release with AppImage, SHA256, and zsync files.
- [ ] ccache reports hits on a subsequent related build.
