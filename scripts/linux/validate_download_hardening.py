#!/usr/bin/env python3
"""Fast source-level regression checks for the downloader/AppImage hardening.

These checks intentionally use only the Python standard library so they can run before CMake
configuration. They complement (not replace) the normal C++ build and the AppImage X11 smoke test.
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"download hardening validation failed: {message}")


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def flatten_keys(value, prefix="") -> set[str]:
    result: set[str] = set()
    if isinstance(value, dict):
        for key, child in value.items():
            path = f"{prefix}/{key}" if prefix else str(key)
            result.add(path)
            result |= flatten_keys(child, path)
    return result


main = read("wiliwili/source/main.cpp")
create_window = main.find('brls::Application::createWindow("wiliwili")')
runtime_hooks = main.find("DownloadManager::instance().initRuntimeHooks()")
require(create_window >= 0 and runtime_hooks > create_window,
        "MPV/download runtime hooks must initialize only after the GLFW window/context")
require(main.find("DownloadManager::instance().shutdown()") > runtime_hooks,
        "download workers must be shut down explicitly before process teardown")

header = read("wiliwili/include/utils/download_manager.hpp")
source = read("wiliwili/source/utils/download_manager.cpp")
require(".detach()" not in source and ".detach(" not in source, "detached download workers are forbidden")
require("std::vector<std::thread> workerThreads" in header, "joined worker pool is missing")
policy = read("wiliwili/include/utils/download_transfer_policy.hpp")
require("responseRangeStartsAt" in source and "rangeResponseMatches" in source and
        "contentRangeStart" in policy,
        "Range resume must use the tested Content-Range transfer policy")
require("diskReservations" in header and "reserveDiskSpace" in source,
        "parallel downloads must reserve free disk space")
path_policy = read("wiliwili/include/utils/download_path_policy.hpp")
require(".wiliwili-download.json" in source and "isManagedTaskDirectory" in source and "download_root" in source and
        "isStrictDescendant" in path_policy,
        "recursive delete must verify task ownership and managed root")
require("std::rename(from.c_str(), to.c_str())" in source and "MOVEFILE_REPLACE_EXISTING" in source,
        "state replacement must use platform atomic-replace primitives")
require("writeJsonAtomic" in source and '".wiliwili-download.json"' in source,
        "per-download metadata/ownership files should use atomic replacement")
require("VERIFYING" in header and "ffprobe" in source and "codec_type,duration" in source,
        "completed media must be stream/duration verified when ffprobe is available")
require("quarantineMediaFile" in source and '".corrupt"' in source,
        "verification failures must quarantine invalid media so Retry can redownload")
require('headers["If-Range"]' in source and '.resume.json' in source,
        "resume should carry ETag/Last-Modified validators with If-Range when available")
require("video.m3u" in source and "#EXTINF:-1" in source,
        "legacy multi-FLV fallback must use a playlist rather than byte concatenation")
require("throttleTokens" in header and "throttleBytes" in source,
        "download limit must be shared across concurrent workers")
require("scanOfflineLibrary" in source and 'filename() != "info.json"' in source,
        "offline library must be reconstructable from per-download metadata")

# The DownloadTask state serializer must not persist expiring signed transport URLs.
to_json_start = header.find("inline void to_json(nlohmann::json& j, const DownloadTask& t)")
from_json_start = header.find("inline void from_json(const nlohmann::json& j, DownloadTask& t)")
require(to_json_start >= 0 and from_json_start > to_json_start, "DownloadTask JSON serializer not found")
serializer = header[to_json_start:from_json_start]
for forbidden in ('{"video_urls"', '{"audio_urls"', '{"flv_segments"'):
    require(forbidden not in serializer, f"signed transport field is persisted: {forbidden}")

# All locale files must expose the same nested key paths, including new hardening UI strings.
i18n_root = ROOT / "resources" / "i18n"
locale_files = sorted(i18n_root.glob("*/wiliwili.json"))
require(bool(locale_files), "no locale files found")
reference_name = locale_files[0].parent.name
reference_data = json.loads(locale_files[0].read_text(encoding="utf-8"))
reference_keys = flatten_keys(reference_data)
for path in locale_files[1:]:
    keys = flatten_keys(json.loads(path.read_text(encoding="utf-8")))
    missing = sorted(reference_keys - keys)
    extra = sorted(keys - reference_keys)
    require(not missing and not extra,
            f"locale key mismatch {path.parent.name} vs {reference_name}; missing={missing[:3]}, extra={extra[:3]}")
for required_key in (
    "setting/tools/download/debug_source",
    "download_manager/stage/verifying",
):
    require(required_key in reference_keys, f"missing i18n key {required_key}")

workflow = read(".github/workflows/appimage.yml")
require("LINUXDEPLOY_VERSION='1-alpha-20251107-1'" in workflow and
        "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d" in workflow,
        "linuxdeploy must be version- and SHA256-pinned")
require("continuous/linuxdeploy" not in workflow, "linuxdeploy continuous channel must not be used")
require('command -v ffprobe' in workflow, "AppImage must bundle ffprobe for offline verification")
require("Smoke-test AppImage startup under X11" in workflow and
        "failed to initialize mpv GL context" in workflow,
        "AppImage startup regression smoke test is missing")
require("Test downloader transfer policy" in workflow and "test_download_transfer_policy.cpp" in workflow,
        "compiled transfer-policy unit test is missing from AppImage CI")
require("softprops/action-gh-release@3d0d9888cb7fd7b750713d6e236d1fcb99157228" in workflow,
        "third-party release action must be commit-pinned")

print(f"download hardening validation passed ({len(locale_files)} locales)")
