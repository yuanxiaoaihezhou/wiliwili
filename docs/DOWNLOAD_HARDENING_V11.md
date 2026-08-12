# Download hardening v11

This revision hardens the offline downloader without changing the existing download UX.

- Runtime download settings are copied to atomics; worker threads do not read the mutable ProgramConfig JSON.
- A fixed worker pool is joined before Borealis/mpv teardown; app exit pauses active downloads instead of cancelling them.
- Concurrent jobs reserve disk space before transfer/mux work.
- HTTP resume accepts byte appends only when a 206 response has a matching Content-Range start. ETag/Last-Modified validators are persisted beside partial tracks and sent with `If-Range` when available.
- Download state uses tmp + backup + atomic replacement, with best-effort fsync on POSIX.
- Recursive deletion requires a task ownership marker (legacy matching info.json is accepted).
- Signed CDN URLs are not stored in download_state.json and are only written to debug_source.json when explicitly enabled.
- Multi-segment legacy FLV uses ffmpeg concat when available, otherwise keeps segments and writes a local generic M3U playlist.
- Retry delays, network waits, metadata waits and ffmpeg/ffprobe subprocesses can be interrupted by pause/cancel/shutdown.
- Download speed limit is a shared token bucket across concurrent tasks instead of a per-task multiplier.
- Completed offline items can be reconstructed by scanning info.json/source.json under the configured library root.
- Completed media is verified with ffprobe when available, including expected stream types and a duration sanity range. Invalid media is preserved as `.corrupt`, progress is reset for the affected track, and Retry redownloads it instead of looping on the same bad file.
- Offline subtitle selection prefers the current app language and uses mpv fixed-length suboption escaping for local paths.
- AppImage startup hooks initialize MPV only after the GLFW window/OpenGL context has been created. CI runs the generated AppImage under Xvfb/Mesa software GL and rejects the exact pre-window MPV/GLFW crash signature.
- Every managed download records its owning download root and marker metadata; recursive deletion is refused outside the verified root.
- A standard-library-only hardening invariant script runs before CMake so critical downloader/startup safety rules fail fast in CI. The HTTP Content-Range decision logic is also extracted into a small dependency-free header and compiled/run as a real C++ unit test in Actions.
- Download Manager progress updates patch only the visible card instead of replacing the entire grid several times per second, preserving controller focus/scroll position.
- Handheld capability detection is centralized and currently recognizes both Legion Go and Steam Deck hardware under SteamOS/gamescope; device-specific layout rules no longer need to leak into individual views.

The persisted task schema is version 3. VERIFYING was appended to the stage enum so older persisted numeric stage values retain their previous meanings.
