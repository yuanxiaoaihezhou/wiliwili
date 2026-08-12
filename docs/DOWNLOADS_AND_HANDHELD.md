# Downloads, offline playback, and handheld mode

## Download flow

Open a video or episode and choose **Download**. On handheld profile the **Y** button opens the same download flow.

For DASH video the flow is:

1. Select quality.
2. Select codec when more than one representation is available (AVC / HEVC / AV1).
3. Select audio quality/track.
4. Choose the current item or the whole multi-part/season collection when available.
5. The task enters Download Manager and progresses through source refresh, space check, video, audio, mux, and offline extras.

The quality/audio selectors show an approximate size when bandwidth and duration are known. Download Manager also checks free disk space before downloading and reserves extra temporary space for lossless ffmpeg muxing.

## Download directory

The default download directory is:

```text
~/Videos/wiliwili
```

It can be changed in **Settings > Tools > Download > Download directory**. `~/...` paths are expanded on desktop Linux/macOS. New tasks use the new directory; existing tasks keep their original directory so resume paths are not broken.

Downloads are grouped by series/video and part/episode. Each item keeps durable source metadata in `source.json` and temporary CDN URLs separately in `debug_source.json`.

Typical layout:

```text
~/Videos/wiliwili/
└── Series or video title/
    └── EP01 - Episode title/
        ├── video.mp4
        ├── cover.jpg
        ├── danmaku.xml
        ├── subtitle.zh-CN.srt
        ├── subtitle.zh-CN.ass
        ├── source.json
        ├── debug_source.json
        ├── info.json
        └── README.txt
```

When ffmpeg is unavailable or muxing fails, the original DASH tracks are kept and the built-in offline player can open the video and audio tracks together.

## Reliability

Partial downloads use HTTP Range resume when the CDN supports it. If a CDN ignores a Range request and returns a full response, the downloader replaces the partial file instead of appending duplicated bytes.

After a suspend/resume, Wi-Fi roam, or expired CDN URL, the worker refreshes the Bilibili play URL and retries while preserving safe partial data. Interrupted active tasks are restored as paused after an application restart so the user remains in control.

## Download Manager

Download Manager supports:

- pause / resume / retry / cancel;
- queue priority up/down;
- configurable 1-4 concurrent downloads;
- normal bandwidth limit;
- a separate lower bandwidth limit while a video is playing;
- opening the local download folder;
- opening the original Bilibili page;
- viewing source/debug metadata;
- deleting a task and its downloaded files;
- direct offline playback after completion.

The **Offline Library** filters completed tasks into a local-only media view.

## Handheld profile

`Auto`, `Desktop`, `TV`, and `Handheld` are separate UI profiles. On Legion Go running SteamOS/gamescope, a fresh configuration defaults to Handheld.

Handheld uses a logical 1280x800 / 16:10 layout scale without forcing the physical framebuffer resolution, keeps TV-style player controls, and defaults to Xbox mappings. This allows SteamOS/gamescope to render at 800p, 1200p, 1600p, or an external display while UI sizing remains comfortable.

Handheld shortcuts added by this feature:

- Home screen **Y**: Download Manager.
- Video detail **Y**: Download current video/episode.
- Existing shoulder/trigger navigation is preserved instead of being overwritten.

## AppImage

The AppImage workflow builds x86_64 Linux with X11 + Wayland, bundles ffmpeg for DASH muxing, uses ccache, publishes `.AppImage`, `.sha256`, and `.zsync` artifacts, and can publish tagged `v*` builds as GitHub Releases.
