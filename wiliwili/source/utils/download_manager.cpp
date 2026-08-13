#include "utils/download_manager.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#elif !defined(__SWITCH__) && !defined(__PSV__) && !defined(PS4)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <cpr/cpr.h>
#include <cpr/filesystem.h>
#include <fmt/format.h>

#include "api/bilibili/util/http.hpp"
#include "api/bilibili/util/uuid.hpp"
#include "bilibili.h"
#include "bilibili/result/video_detail_result.h"
#include "utils/config_helper.hpp"
#include "utils/download_transfer_policy.hpp"
#include "utils/download_path_policy.hpp"

using namespace brls::literals;

#ifdef _WIN32
static constexpr char PATH_SEP = '\\';
#else
static constexpr char PATH_SEP = '/';
#endif

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + PATH_SEP + b;
}

static std::string sanitizePathComponent(std::string value) {
    for (char& c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        else if (u < 32) c = '_';
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    if (value.empty()) value = "video";
    if (value.size() > 120) value.resize(120);
    return value;
}

static std::string subtitleSafeName(std::string value) {
    for (char& c : value) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    if (value.empty()) value = "subtitle";
    return value;
}

static std::string srtTime(float seconds) {
    if (seconds < 0) seconds = 0;
    const int totalMs = static_cast<int>(seconds * 1000.0f + 0.5f);
    const int ms = totalMs % 1000;
    const int totalSec = totalMs / 1000;
    const int sec = totalSec % 60;
    const int totalMin = totalSec / 60;
    const int min = totalMin % 60;
    const int hour = totalMin / 60;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hour << ':' << std::setw(2) << min << ':'
        << std::setw(2) << sec << ',' << std::setw(3) << ms;
    return out.str();
}

static std::string assTime(float seconds) {
    if (seconds < 0) seconds = 0;
    const int totalCs = static_cast<int>(seconds * 100.0f + 0.5f);
    const int cs = totalCs % 100;
    const int totalSec = totalCs / 100;
    const int sec = totalSec % 60;
    const int totalMin = totalSec / 60;
    const int min = totalMin % 60;
    const int hour = totalMin / 60;
    std::ostringstream out;
    out << hour << ':' << std::setfill('0') << std::setw(2) << min << ':' << std::setw(2) << sec << '.' << std::setw(2) << cs;
    return out.str();
}

static std::string escapeAss(std::string text) {
    size_t pos = 0;
    while ((pos = text.find('\n', pos)) != std::string::npos) { text.replace(pos, 1, "\\N"); pos += 2; }
    return text;
}

static bool moveReplace(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return ::MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    // POSIX rename atomically replaces an existing destination on the same filesystem. Do not
    // remove the old state/media file first: that creates a crash window with no valid file.
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

static bool copyReplace(const std::string& from, const std::string& to) {
    try {
        cpr::fs::copy_file(cpr::fs::path(from), cpr::fs::path(to),
                                   cpr::fs::copy_options::overwrite_existing);
        return true;
    } catch (...) {
        return false;
    }
}

static void quarantineMediaFile(const std::string& path) {
    if (path.empty()) return;
    try {
        const auto corruptPath = path + ".corrupt";
        if (cpr::fs::exists(corruptPath)) cpr::fs::remove(corruptPath);
        if (cpr::fs::exists(path)) {
            if (!moveReplace(path, corruptPath)) {
                // If a cross-filesystem or platform-specific rename ever fails, preserve safety: do
                // not delete the suspect media. Retry will fail visibly rather than silently losing it.
                brls::Logger::warning("DownloadManager: could not quarantine invalid media {}", path);
                return;
            }
        }
        const auto resumeMeta = path + ".resume.json";
        const auto requestPart = path + ".request.part";
        if (cpr::fs::exists(resumeMeta)) cpr::fs::remove(resumeMeta);
        if (cpr::fs::exists(requestPart)) cpr::fs::remove(requestPart);
    } catch (const std::exception& e) {
        brls::Logger::warning("DownloadManager: failed to quarantine invalid media {}: {}", path, e.what());
    }
}

static void syncFileBestEffort(const std::string& path) {
#if !defined(_WIN32) && !defined(__SWITCH__) && !defined(__PSV__) && !defined(PS4)
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

static void syncDirectoryBestEffort(const std::string& path) {
#if !defined(_WIN32) && !defined(__SWITCH__) && !defined(__PSV__) && !defined(PS4)
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

static bool writeJsonAtomic(const std::string& path, const nlohmann::json& value) {
    try {
        const auto temp = path + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            const auto data = value.dump(2);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            out.flush();
            if (!out.good()) return false;
        }
        syncFileBestEffort(temp);
        if (!moveReplace(temp, path)) {
            try { if (cpr::fs::exists(temp)) cpr::fs::remove(temp); } catch (...) {}
            return false;
        }
        syncDirectoryBestEffort(cpr::fs::path(path).parent_path().string());
        return true;
    } catch (...) {
        return false;
    }
}


static bool isStrictPathDescendant(const cpr::fs::path& child, const cpr::fs::path& root) {
    return wiliwili::download_policy::isStrictDescendant(child, root);
}

static std::string urlHost(const std::string& url) {
    auto start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    auto end = url.find_first_of("/?#", start);
    auto host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    auto at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    return host.empty() ? std::string{"media host"} : host;
}

static std::string responseHeader(const cpr::Response& response, std::string expectedName) {
    std::transform(expectedName.begin(), expectedName.end(), expectedName.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto& header : response.header) {
        std::string name = header.first;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (name == expectedName) return header.second;
    }
    return {};
}

static bool responseRangeStartsAt(const cpr::Response& response, int64_t expected) {
    return wiliwili::download_policy::rangeResponseMatches(
        response.status_code, responseHeader(response, "content-range"), expected);
}

static bool cancelledOrPaused(const DownloadTask& task) {
    return (task.cancelFlag && task.cancelFlag->load()) || (task.pauseFlag && task.pauseFlag->load());
}

static std::vector<std::string> mediaUrls(const bilibili::DashMedia& media) {
    std::vector<std::string> urls;
    if (!media.base_url.empty()) urls.emplace_back(media.base_url);
    urls.insert(urls.end(), media.backup_url.begin(), media.backup_url.end());
    return urls;
}

static std::string shellQuote(const std::string& value) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : value) out += (c == '\"') ? "\\\"" : std::string(1, c);
    return out + "\"";
#else
    std::string out = "'";
    for (char c : value) out += (c == '\'') ? "'\\''" : std::string(1, c);
    return out + "'";
#endif
}

static bool hasTool(const char* name) {
#if defined(__SWITCH__) || defined(__PSV__) || defined(PS4)
    (void)name;
    return false;
#elif defined(_WIN32)
    return std::system((std::string("where ") + name + " >nul 2>nul").c_str()) == 0;
#else
    return std::system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

static bool hasFfmpeg() { return hasTool("ffmpeg"); }
static bool hasFfprobe() { return hasTool("ffprobe"); }

static std::string makeTaskId(const DownloadTask& task) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return bilibili::genUUID(task.bvid + std::to_string(task.cid) + std::to_string(now));
}

DownloadManager::DownloadManager() {
    throttleLastRefill = std::chrono::steady_clock::now();
    ensureWorkers();
}

DownloadManager::~DownloadManager() { shutdown(); }

void DownloadManager::initRuntimeHooks() {
    if (runtimeHooksInitialized || shuttingDown.load()) return;
    // IMPORTANT: caller must invoke this after Application::createWindow(). MPV_E lazily constructs
    // MPVCore, whose OpenGL renderer asks GLFW for GL entry points and therefore needs a current
    // window/context. Constructing it before createWindow caused the SteamOS AppImage startup crash.
    mpvEventSubscription = MPV_E->subscribe([this](MpvEventEnum event) {
        if (event == MPV_RESUME || event == MPV_LOADED || event == START_FILE) playbackActive.store(true);
        else if (event == MPV_PAUSE || event == MPV_IDLE || event == MPV_STOP || event == END_OF_FILE || event == MPV_FILE_ERROR)
            playbackActive.store(false);
        throttleCv.notify_all();
    });
    runtimeHooksInitialized = true;
}

void DownloadManager::reloadRuntimeConfig() {
    auto parseLimit = [](const std::string& value, int64_t fallback) -> int64_t {
        try {
            const double mb = std::stod(value);
            return mb > 0 ? static_cast<int64_t>(mb * 1024.0 * 1024.0) : 0;
        } catch (...) {
            return fallback;
        }
    };
    auto& conf = ProgramConfig::instance();
    int concurrency = 2;
    try { concurrency = std::stoi(conf.getSettingItem(SettingItem::DOWNLOAD_CONCURRENCY, std::string{"2"})); } catch (...) {}
    concurrentLimit.store(std::max(1, std::min(4, concurrency)));
    normalSpeedLimit.store(parseLimit(conf.getSettingItem(SettingItem::DOWNLOAD_SPEED_LIMIT, std::string{"0"}), 0));
    playbackSpeedLimit.store(parseLimit(conf.getSettingItem(SettingItem::DOWNLOAD_PLAYBACK_SPEED_LIMIT, std::string{"5"}),
                                       5LL * 1024LL * 1024LL));
    runtimeDownloadCover.store(conf.getBoolOption(SettingItem::DOWNLOAD_COVER));
    runtimeDownloadDanmaku.store(conf.getBoolOption(SettingItem::DOWNLOAD_DANMAKU));
    runtimeDownloadSubtitles.store(conf.getBoolOption(SettingItem::DOWNLOAD_SUBTITLES));
    runtimeDebugSource.store(conf.getBoolOption(SettingItem::DOWNLOAD_DEBUG_SOURCE));
    workerCv.notify_all();
    throttleCv.notify_all();
}

void DownloadManager::shutdown() {
    bool expected = false;
    if (!shuttingDown.compare_exchange_strong(expected, true)) return;

    if (runtimeHooksInitialized) {
        MPV_E->unsubscribe(mpvEventSubscription);
        runtimeHooksInitialized = false;
    }
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        // Application exit is a resumable pause, not a user cancellation. Active workers stop,
        // flush validated partials, and persist PAUSED so the next launch can resume explicitly.
        for (auto& task : tasks) if (task.pauseFlag) task.pauseFlag->store(true);
    }
    workerCv.notify_all();
    throttleCv.notify_all();
    for (auto& worker : workerThreads) if (worker.joinable()) worker.join();
    workerThreads.clear();
    saveState();
}

std::string DownloadManager::defaultDownloadDir() {
    auto& conf = ProgramConfig::instance();
    const auto home = conf.getHomePath();
    const auto fallback = joinPath(joinPath(home, "Videos"), "wiliwili");
    auto configured = conf.getSettingItem(SettingItem::VIDEO_DOWNLOAD_PATH, fallback);
    if (configured.empty()) return fallback;
    // Text settings commonly use ~/... on Linux. Resolve it here so both the UI and worker
    // use the same concrete directory while still preserving arbitrary absolute/custom paths.
    if (configured == "~") return home;
    if (configured.size() > 2 && configured[0] == '~' && (configured[1] == '/' || configured[1] == '\\'))
        return home + configured.substr(1);
    return configured;
}

int64_t DownloadManager::estimateBytes(const DownloadTask& task) {
    if (task.estimated_bytes > 0) return task.estimated_bytes;
    if (!task.flv_segments.empty()) {
        uint64_t total = 0;
        for (const auto& seg : task.flv_segments) total += seg.size;
        if (total > 0) return static_cast<int64_t>(total);
    }
    if (task.duration_seconds > 0) {
        const uint64_t bandwidth = static_cast<uint64_t>(task.video_bandwidth) + static_cast<uint64_t>(task.audio_bandwidth);
        if (bandwidth > 0) return static_cast<int64_t>((bandwidth / 8.0) * task.duration_seconds * 1.04);
    }
    return task.total_bytes + task.audio_total_bytes;
}

int64_t DownloadManager::availableBytes(const std::string& path) {
    try {
        cpr::fs::path p(path);
        while (!p.empty() && !cpr::fs::exists(p)) p = p.parent_path();
        if (p.empty()) return -1;
        return static_cast<int64_t>(cpr::fs::space(p).available);
    } catch (...) { return -1; }
}

std::string DownloadManager::playableVideoPath(const DownloadTask& task) {
    if (task.dir.empty()) return {};
    if (!task.output_file.empty() && cpr::fs::exists(joinPath(task.dir, task.output_file))) return joinPath(task.dir, task.output_file);
    if (!task.video_file.empty() && cpr::fs::exists(joinPath(task.dir, task.video_file))) return joinPath(task.dir, task.video_file);
    return {};
}

std::string DownloadManager::playableAudioPath(const DownloadTask& task) {
    if (task.muxed || task.audio_file.empty() || task.dir.empty()) return {};
    const auto path = joinPath(task.dir, task.audio_file);
    return cpr::fs::exists(path) ? path : std::string{};
}

void DownloadManager::loadState() {
    reloadRuntimeConfig();
    const auto dir = ProgramConfig::instance().getConfigDir();
    const auto statePath = joinPath(dir, "download_state.json");
    const std::vector<std::string> candidates{statePath, statePath + ".tmp", statePath + ".bak", statePath + ".bak.tmp"};

    std::vector<DownloadTask> loaded;
    std::string loadedFrom;
    for (const auto& path : candidates) {
        if (!cpr::fs::exists(path)) continue;
        try {
            std::ifstream f(path);
            if (!f.is_open()) continue;
            nlohmann::json j; f >> j;
            loaded = j.get<std::vector<DownloadTask>>();
            loadedFrom = path;
            break;
        } catch (const std::exception& e) {
            brls::Logger::warning("DownloadManager: cannot read state candidate {}: {}", path, e.what());
        }
    }

    if (!loaded.empty() || !loadedFrom.empty()) {
        const auto currentDownloadRoot = defaultDownloadDir();
        cpr::fs::path canonicalCurrentRoot;
        try { canonicalCurrentRoot = cpr::fs::weakly_canonical(cpr::fs::path(currentDownloadRoot)); } catch (...) {}
        for (auto& t : loaded) {
            // Never auto-resume after a process restart. The user can resume explicitly, at which
            // point refreshSource() obtains fresh signed CDN URLs before any Range request.
            if (t.status == DownloadTaskStatus::DOWNLOADING || t.status == DownloadTaskStatus::PENDING)
                t.status = DownloadTaskStatus::PAUSED;
            if (t.download_root.empty() && !t.dir.empty() && !canonicalCurrentRoot.empty()) {
                try {
                    const auto canonicalTaskDir = cpr::fs::weakly_canonical(cpr::fs::path(t.dir));
                    if (isStrictPathDescendant(canonicalTaskDir, canonicalCurrentRoot)) t.download_root = currentDownloadRoot;
                } catch (...) {}
            }
            t.video_urls.clear();
            t.audio_urls.clear();
            t.flv_segments.clear();
            t.cancelFlag = std::make_shared<std::atomic<bool>>(false);
            t.pauseFlag = std::make_shared<std::atomic<bool>>(false);
            t.schema_version = 3;
        }
        {
            std::lock_guard<std::mutex> lock(tasksMutex);
            tasks = std::move(loaded);
        }
        brls::Logger::info("DownloadManager: loaded {} task(s) from {}", tasks.size(), loadedFrom);
        if (loadedFrom != statePath) {
            // Do not let saveState() overwrite a good .bak with the corrupt primary that forced
            // recovery. Preserve that primary as .corrupt for diagnostics, then rebuild primary.
            try {
                if (cpr::fs::exists(statePath)) moveReplace(statePath, statePath + ".corrupt");
            } catch (...) {}
            saveState();
        }
    }

    // The library is intentionally reconstructable from per-download metadata. This recovers
    // completed downloads if download_state.json was deleted/corrupted or copied from another PC.
    scanOfflineLibrary();
}

void DownloadManager::saveState() {
    if (ProgramConfig::instance().getConfigDir().empty()) return;
    try {
        const auto dir = ProgramConfig::instance().getConfigDir();
        cpr::fs::create_directories(dir);
        std::lock_guard<std::mutex> stateLock(stateFileMutex);
        std::vector<DownloadTask> snapshot;
        {
            std::lock_guard<std::mutex> lock(tasksMutex);
            snapshot = tasks;
        }
        for (auto& task : snapshot) {
            task.schema_version = 3;
            task.video_urls.clear();
            task.audio_urls.clear();
            task.flv_segments.clear();
        }

        const auto statePath = joinPath(dir, "download_state.json");
        const auto tempPath = statePath + ".tmp";
        const auto backupPath = statePath + ".bak";
        const auto backupTempPath = backupPath + ".tmp";
        {
            std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return;
            const auto data = nlohmann::json(snapshot).dump(2);
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
            f.flush();
            if (!f.good()) return;
        }

        // Preserve the last known-good state, flush the new file, then atomically replace the
        // primary state. fsync is best-effort on POSIX so sudden power loss cannot leave only a
        // directory entry pointing at unwritten data.
        syncFileBestEffort(tempPath);
        if (cpr::fs::exists(statePath)) {
            if (copyReplace(statePath, backupTempPath)) {
                syncFileBestEffort(backupTempPath);
                if (!moveReplace(backupTempPath, backupPath)) {
                    brls::Logger::warning("DownloadManager: cannot atomically refresh state backup");
                    try { cpr::fs::remove(backupTempPath); } catch (...) {}
                }
            }
        }
        if (!moveReplace(tempPath, statePath)) {
            brls::Logger::error("DownloadManager: cannot atomically replace state file");
            try { cpr::fs::remove(tempPath); } catch (...) {}
        } else {
            syncDirectoryBestEffort(dir);
        }
    } catch (const std::exception& e) {
        brls::Logger::error("DownloadManager: save state failed: {}", e.what());
    }
}

void DownloadManager::scanOfflineLibrary() {
    const auto root = defaultDownloadDir();
    if (root.empty() || !cpr::fs::exists(root)) return;

    std::vector<DownloadTask> recovered;
    try {
        for (const auto& entry : cpr::fs::recursive_directory_iterator(
                 cpr::fs::path(root), cpr::fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file() || entry.path().filename() != "info.json") continue;
            try {
                std::ifstream f(entry.path());
                if (!f.is_open()) continue;
                nlohmann::json info; f >> info;
                if (!info.value("completed", false)) continue;

                const auto itemDir = entry.path().parent_path();
                nlohmann::json source;
                bool sourceOwned = false;
                const auto sourcePath = itemDir / "source.json";
                if (cpr::fs::exists(sourcePath)) {
                    try {
                        std::ifstream sf(sourcePath);
                        sf >> source;
                        sourceOwned = source.value("app", "") == "wiliwili";
                    } catch (...) {}
                }
                bool markerOwned = false;
                const auto markerPath = itemDir / ".wiliwili-download.json";
                if (cpr::fs::exists(markerPath)) {
                    try {
                        std::ifstream mf(markerPath);
                        nlohmann::json marker; mf >> marker;
                        markerOwned = marker.value("app", "") == "wiliwili";
                    } catch (...) {}
                }
                if (!sourceOwned && !markerOwned) continue;

                DownloadTask task;
                task.schema_version = 3;
                task.id = info.value("id", "");
                task.title = info.value("title", "");
                task.series_title = info.value("series_title", task.title);
                task.part_title = info.value("part_title", "");
                task.part_index = info.value("part_index", 0);
                task.owner_name = info.value("owner_name", "");
                task.cover_url = info.value("cover_url", "");
                task.bvid = info.value("bvid", "");
                task.aid = info.value("aid", uint64_t{0});
                task.cid = info.value("cid", uint64_t{0});
                task.quality = info.value("quality", 0);
                task.quality_desc = info.value("quality_desc", "");
                task.audio_id = info.value("audio_id", 0);
                task.audio_desc = info.value("audio_desc", "");
                task.is_dash = info.value("is_dash", false);
                task.is_pgc = info.value("is_pgc", false);
                task.duration_seconds = info.value("duration_seconds", 0);
                task.estimated_bytes = info.value("estimated_bytes", int64_t{0});
                task.downloaded_bytes = info.value("downloaded_bytes", int64_t{0});
                task.total_bytes = info.value("total_bytes", int64_t{0});
                task.audio_downloaded_bytes = info.value("audio_downloaded_bytes", int64_t{0});
                task.audio_total_bytes = info.value("audio_total_bytes", int64_t{0});
                task.download_root = root;
                task.dir = itemDir.string();
                task.video_file = info.value("video_file", "");
                task.audio_file = info.value("audio_file", "");
                task.output_file = info.value("output_file", "");
                task.muxed = info.value("muxed", false);
                task.created_at = info.value("created_at", int64_t{0});
                task.finished_at = info.value("finished_at", int64_t{0});
                task.status = DownloadTaskStatus::COMPLETED;
                task.stage = DownloadTaskStage::COMPLETED;
                task.cancelFlag = std::make_shared<std::atomic<bool>>(false);
                task.pauseFlag = std::make_shared<std::atomic<bool>>(false);

                if (sourceOwned) {
                    task.source_page_url = source.value("source_page_url", "");
                    if (source.contains("video") && source["video"].is_object()) {
                        const auto& video = source["video"];
                        if (task.quality_desc.empty()) task.quality_desc = video.value("quality_desc", "");
                        task.video_codec_id = video.value("codec_id", 0);
                        task.video_bandwidth = video.value("bandwidth", 0u);
                        task.video_width = video.value("width", 0);
                        task.video_height = video.value("height", 0);
                    }
                    if (source.contains("audio") && source["audio"].is_object()) {
                        const auto& audio = source["audio"];
                        task.audio_codec_id = audio.value("codec_id", 0);
                        task.audio_bandwidth = audio.value("bandwidth", 0u);
                    }
                }
                if (task.id.empty()) task.id = bilibili::genUUID(task.dir + std::to_string(task.cid));
                if (!playableVideoPath(task).empty()) recovered.emplace_back(std::move(task));
            } catch (...) {}
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("DownloadManager: offline library scan failed: {}", e.what());
    }

    if (recovered.empty()) return;
    size_t added = 0;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& candidate : recovered) {
            const auto duplicate = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& existing) {
                return (!candidate.id.empty() && existing.id == candidate.id) ||
                       (!candidate.dir.empty() && existing.dir == candidate.dir) ||
                       (candidate.cid != 0 && existing.cid == candidate.cid && existing.bvid == candidate.bvid &&
                        existing.status == DownloadTaskStatus::COMPLETED);
            });
            if (duplicate == tasks.end()) {
                tasks.emplace_back(std::move(candidate));
                ++added;
            }
        }
    }
    if (added > 0) {
        brls::Logger::info("DownloadManager: recovered {} completed offline download(s) from metadata", added);
        saveState();
    }
}

void DownloadManager::addTask(DownloadTask task) {
    if (task.id.empty()) task.id = makeTaskId(task);
    if (task.created_at == 0) task.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    task.schema_version = 3;
    if (task.series_title.empty()) task.series_title = task.title.empty() ? (task.bvid.empty() ? "Bilibili" : task.bvid) : task.title;
    if (task.part_title.empty()) task.part_title = task.title;
    if (task.estimated_bytes <= 0) task.estimated_bytes = estimateBytes(task);
    if (task.download_root.empty()) task.download_root = defaultDownloadDir();
    if (task.dir.empty()) {
        const auto& base = task.download_root;
        cpr::fs::create_directories(base);
        // Keep all parts/episodes of the same logical series in one folder. Individual BVID/CID
        // provenance remains in source.json; leaf collisions are disambiguated with CID.
        std::string seriesStem = sanitizePathComponent(task.series_title);
        auto seriesDir = joinPath(base, seriesStem);
        cpr::fs::create_directories(seriesDir);
        std::string prefix;
        if (task.part_index > 0) prefix = fmt::format("{}{:02d} - ", task.is_pgc ? "EP" : "P", task.part_index);
        auto leaf = prefix + sanitizePathComponent(task.part_title.empty() ? task.title : task.part_title);
        auto candidate = joinPath(seriesDir, leaf);
        if (cpr::fs::exists(candidate)) candidate += "_" + std::to_string(task.cid);
        task.dir = candidate;
    }
    if (task.video_file.empty()) task.video_file = task.is_dash ? "video.m4s" : "video.flv";
    if (task.is_dash && (task.audio_id != 0 || !task.audio_urls.empty()) && task.audio_file.empty()) task.audio_file = "audio.m4s";
    task.status = DownloadTaskStatus::PENDING;
    task.stage = DownloadTaskStage::QUEUED;
    task.error_message.clear();
    task.cancelFlag = std::make_shared<std::atomic<bool>>(false);
    task.pauseFlag = std::make_shared<std::atomic<bool>>(false);
    cpr::fs::create_directories(task.dir);
    saveMetadata(task, false);
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        tasks.emplace_back(std::move(task));
    }
    saveState();
    startNextTask();
}

void DownloadManager::addTasks(std::vector<DownloadTask> batch) {
    if (batch.empty()) return;
    for (auto& task : batch) {
        if (task.id.empty()) task.id = makeTaskId(task);
        if (task.created_at == 0) task.created_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        task.schema_version = 3;
        if (task.series_title.empty()) task.series_title = task.title.empty() ? (task.bvid.empty() ? "Bilibili" : task.bvid) : task.title;
        if (task.part_title.empty()) task.part_title = task.title;
        if (task.estimated_bytes <= 0) task.estimated_bytes = estimateBytes(task);
        if (task.download_root.empty()) task.download_root = defaultDownloadDir();
        if (task.dir.empty()) {
            const auto& base = task.download_root;
            cpr::fs::create_directories(base);
            // Keep all parts/episodes of the same logical series in one folder.
            std::string seriesStem = sanitizePathComponent(task.series_title);
            auto seriesDir = joinPath(base, seriesStem);
            cpr::fs::create_directories(seriesDir);
            std::string prefix;
            if (task.part_index > 0) prefix = fmt::format("{}{:02d} - ", task.is_pgc ? "EP" : "P", task.part_index);
            auto leaf = prefix + sanitizePathComponent(task.part_title.empty() ? task.title : task.part_title);
            auto candidate = joinPath(seriesDir, leaf);
            if (cpr::fs::exists(candidate)) candidate += "_" + std::to_string(task.cid);
            task.dir = candidate;
        }
        if (task.video_file.empty()) task.video_file = task.is_dash ? "video.m4s" : "video.flv";
        if (task.is_dash && (task.audio_id != 0 || !task.audio_urls.empty()) && task.audio_file.empty()) task.audio_file = "audio.m4s";
        task.status = DownloadTaskStatus::PENDING;
        task.stage = DownloadTaskStage::QUEUED;
        task.error_message.clear();
        task.cancelFlag = std::make_shared<std::atomic<bool>>(false);
        task.pauseFlag = std::make_shared<std::atomic<bool>>(false);
        cpr::fs::create_directories(task.dir);
        saveMetadata(task, false);
    }
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& task : batch) tasks.emplace_back(std::move(task));
    }
    saveState();
    startNextTask();
}

void DownloadManager::pauseTask(const std::string& id) {
    bool requested = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == id && t.status == DownloadTaskStatus::DOWNLOADING) {
            if (t.pauseFlag) t.pauseFlag->store(true);
            requested = true;
            break;
        }
    }
    // Keep it DOWNLOADING until the worker has fully stopped, so the concurrency slot
    // is not reused while cpr is still unwinding its callbacks.
    if (requested) saveState();
}

void DownloadManager::resumeTask(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == id && t.status == DownloadTaskStatus::PAUSED) {
            t.status = DownloadTaskStatus::PENDING;
            t.stage = DownloadTaskStage::QUEUED;
            t.error_message.clear();
            t.cancelFlag = std::make_shared<std::atomic<bool>>(false);
            t.pauseFlag = std::make_shared<std::atomic<bool>>(false);
            break;
        }
    }
    saveState(); taskStatusChangedEvent.fire(id); startNextTask();
}

void DownloadManager::retryTask(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == id && (t.status == DownloadTaskStatus::FAILED || t.status == DownloadTaskStatus::CANCELLED)) {
            t.status = DownloadTaskStatus::PENDING;
            t.stage = DownloadTaskStage::QUEUED;
            t.error_message.clear();
            t.cancelFlag = std::make_shared<std::atomic<bool>>(false);
            t.pauseFlag = std::make_shared<std::atomic<bool>>(false);
            break;
        }
    }
    saveState(); taskStatusChangedEvent.fire(id); startNextTask();
}

void DownloadManager::cancelTask(const std::string& id) {
    bool active = false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == id) {
            if (t.status == DownloadTaskStatus::DOWNLOADING) {
                if (t.cancelFlag) t.cancelFlag->store(true);
                active = true;
                changed = true;
            } else if (t.status == DownloadTaskStatus::PENDING || t.status == DownloadTaskStatus::PAUSED ||
                       t.status == DownloadTaskStatus::FAILED) {
                if (t.cancelFlag) t.cancelFlag->store(true);
                t.status = DownloadTaskStatus::CANCELLED;
                changed = true;
            }
            break;
        }
    }
    if (!changed) return;
    saveState();
    if (!active) {
        taskStatusChangedEvent.fire(id);
        startNextTask();
    }
    // Active tasks transition to CANCELLED in runTask only after their worker has stopped.
}

bool DownloadManager::isManagedTaskDirectory(const DownloadTask& task) const {
    if (task.dir.empty()) return false;
    try {
        const auto path = cpr::fs::weakly_canonical(cpr::fs::path(task.dir));
        if (!cpr::fs::exists(path) || !cpr::fs::is_directory(path)) return false;
        const auto rootPath = path.root_path();
        if (path == rootPath) return false;
        const auto home = cpr::fs::weakly_canonical(cpr::fs::path(ProgramConfig::instance().getHomePath()));
        if (path == home) return false;

        // New tasks remember the exact download root that owned them. This prevents a corrupted
        // state file from turning remove_all() into a deletion primitive outside wiliwili's tree,
        // even after the user changes the global download directory later.
        auto verifyRoot = [&](const std::string& managedRoot) -> bool {
            if (managedRoot.empty()) return true;  // legacy tasks are verified by identity metadata below
            try {
                const auto canonicalRoot = cpr::fs::weakly_canonical(cpr::fs::path(managedRoot));
                return isStrictPathDescendant(path, canonicalRoot);
            } catch (...) {
                return false;
            }
        };

        const auto markerPath = path / ".wiliwili-download.json";
        if (cpr::fs::exists(markerPath)) {
            std::ifstream f(markerPath);
            nlohmann::json marker; f >> marker;
            if (marker.value("app", "") != "wiliwili" || marker.value("task_id", "") != task.id ||
                (task.cid != 0 && marker.value("cid", uint64_t{0}) != task.cid)) return false;
            const auto managedRoot = !task.download_root.empty() ? task.download_root : marker.value("download_root", "");
            return verifyRoot(managedRoot);
        }

        // Backward compatibility for v9/v10 downloads created before the marker/root fields. The
        // matching task id + CID in info.json is required; new tasks never rely on this fallback.
        const auto infoPath = path / "info.json";
        if (!cpr::fs::exists(infoPath)) return false;
        std::ifstream f(infoPath);
        nlohmann::json info; f >> info;
        if (info.value("id", "") != task.id ||
            (task.cid != 0 && info.value("cid", uint64_t{0}) != task.cid)) return false;
        const auto managedRoot = !task.download_root.empty() ? task.download_root : info.value("download_root", "");
        return verifyRoot(managedRoot);
    } catch (...) {
        return false;
    }
}

void DownloadManager::deleteTask(const std::string& id) {
    DownloadTask task;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& t) { return t.id == id; });
        if (it == tasks.end() || it->status == DownloadTaskStatus::DOWNLOADING || runningTasks.count(id) != 0) return;
        task = *it;
    }

    if (!task.dir.empty() && cpr::fs::exists(task.dir)) {
        if (!isManagedTaskDirectory(task)) {
            brls::Logger::error("DownloadManager: refusing recursive deletion of unverified task directory: {}", task.dir);
            return;
        }
        try {
            cpr::fs::remove_all(task.dir);
        } catch (const std::exception& e) {
            brls::Logger::error("DownloadManager: cannot delete task directory: {}", e.what());
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& t) { return t.id == id; });
        if (it != tasks.end()) tasks.erase(it);
    }
    saveState();
    taskStatusChangedEvent.fire(id);
    startNextTask();
}


void DownloadManager::moveTaskUp(const std::string& id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& t) { return t.id == id; });
        if (it != tasks.end() && it != tasks.begin() &&
            (it->status == DownloadTaskStatus::PENDING || it->status == DownloadTaskStatus::PAUSED)) {
            auto prev = it; --prev;
            if (prev->status == DownloadTaskStatus::PENDING || prev->status == DownloadTaskStatus::PAUSED) {
                std::iter_swap(prev, it); changed = true;
            }
        }
    }
    if (changed) { saveState(); taskStatusChangedEvent.fire(id); }
}

void DownloadManager::moveTaskDown(const std::string& id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& t) { return t.id == id; });
        if (it != tasks.end() && (it->status == DownloadTaskStatus::PENDING || it->status == DownloadTaskStatus::PAUSED)) {
            auto next = it; ++next;
            if (next != tasks.end() && (next->status == DownloadTaskStatus::PENDING || next->status == DownloadTaskStatus::PAUSED)) {
                std::iter_swap(it, next); changed = true;
            }
        }
    }
    if (changed) { saveState(); taskStatusChangedEvent.fire(id); }
}

std::vector<DownloadTask> DownloadManager::getTasksSnapshot() const {
    std::lock_guard<std::mutex> lock(tasksMutex); return tasks;
}
bool DownloadManager::getTaskSnapshot(const std::string& id, DownloadTask& out) const {
    std::lock_guard<std::mutex> lock(tasksMutex);
    for (const auto& t : tasks) if (t.id == id) { out = t; return true; }
    return false;
}
bool DownloadManager::hasIncompleteDownloads() const {
    std::lock_guard<std::mutex> lock(tasksMutex);
    for (const auto& t : tasks) if (t.status == DownloadTaskStatus::PENDING || t.status == DownloadTaskStatus::DOWNLOADING || t.status == DownloadTaskStatus::PAUSED) return true;
    return false;
}
bool DownloadManager::hasCompletedTask(const std::string& bvid, uint64_t cid) const {
    std::lock_guard<std::mutex> lock(tasksMutex);
    for (const auto& t : tasks) if (t.bvid == bvid && t.cid == cid && t.status == DownloadTaskStatus::COMPLETED) return true;
    return false;
}


void DownloadManager::ensureTaskCover(const DownloadTask& snapshot) {
    if (!snapshot.cover_url.empty() || snapshot.id.empty()) return;
    if (snapshot.bvid.empty() && snapshot.aid == 0) return;

    {
        std::lock_guard<std::mutex> lock(coverRequestMutex);
        if (!coverRequests.insert(snapshot.id).second) return;
    }

    const auto id = snapshot.id;
    auto finish = [this, id](const std::string& cover) {
        DownloadTask updated;
        bool changed = false;
        if (!cover.empty() && !shuttingDown.load()) {
            std::lock_guard<std::mutex> lock(tasksMutex);
            auto it = std::find_if(tasks.begin(), tasks.end(),
                                   [&](const DownloadTask& task) { return task.id == id; });
            if (it != tasks.end() && it->cover_url.empty()) {
                it->cover_url = cover;
                updated = *it;
                changed = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(coverRequestMutex);
            coverRequests.erase(id);
        }
        if (!changed) return;

        saveMetadata(updated, updated.status == DownloadTaskStatus::COMPLETED);
        saveState();
        brls::sync([this, id]() { taskStatusChangedEvent.fire(id); });
    };

    auto failure = [finish](const std::string&, int) { finish(std::string{}); };
    if (!snapshot.bvid.empty()) {
        BILI::get_video_detail(
            snapshot.bvid,
            [finish](const bilibili::VideoDetailResult& result) { finish(result.pic); },
            failure);
    } else {
        BILI::get_video_detail(
            snapshot.aid,
            [finish](const bilibili::VideoDetailResult& result) { finish(result.pic); },
            failure);
    }
}

int DownloadManager::maxConcurrent() const { return concurrentLimit.load(); }

int64_t DownloadManager::effectiveSpeedLimit() const {
    const int64_t normal = normalSpeedLimit.load();
    if (!playbackActive.load()) return normal;
    const int64_t duringPlayback = playbackSpeedLimit.load();
    if (duringPlayback <= 0) return normal;
    if (normal <= 0) return duringPlayback;
    return std::min(normal, duringPlayback);
}

void DownloadManager::ensureWorkers() {
    if (!workerThreads.empty()) return;
    workerThreads.reserve(4);
    for (int i = 0; i < 4; ++i) workerThreads.emplace_back([this]() { workerLoop(); });
}

void DownloadManager::startNextTask() {
    if (shuttingDown.load()) return;
    workerCv.notify_all();
}

void DownloadManager::workerLoop() {
    while (!shuttingDown.load()) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(tasksMutex);
            if (static_cast<int>(runningTasks.size()) < maxConcurrent()) {
                auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& task) {
                    return task.status == DownloadTaskStatus::PENDING && runningTasks.count(task.id) == 0;
                });
                if (it != tasks.end()) {
                    it->status = DownloadTaskStatus::DOWNLOADING;
                    it->stage = DownloadTaskStage::REFRESHING_SOURCE;
                    it->error_message.clear();
                    runningTasks.insert(it->id);
                    id = it->id;
                }
            }
        }

        if (id.empty()) {
            std::unique_lock<std::mutex> waitLock(workerMutex);
            workerCv.wait_for(waitLock, std::chrono::milliseconds(250));
            continue;
        }

        saveState();
        try {
            runTask(id);
        } catch (const std::exception& e) {
            handleWorkerFailure(id, std::string("Download worker error: ") + e.what());
        } catch (...) {
            handleWorkerFailure(id, "Unexpected download worker error");
        }
    }
}

void DownloadManager::handleWorkerFailure(const std::string& id, const std::string& error) {
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        owned = runningTasks.erase(id) > 0;
        if (owned) {
            for (auto& task : tasks) {
                if (task.id != id) continue;
                if (task.cancelFlag && task.cancelFlag->load()) task.status = DownloadTaskStatus::CANCELLED;
                else if (task.pauseFlag && task.pauseFlag->load()) task.status = DownloadTaskStatus::PAUSED;
                else {
                    task.status = DownloadTaskStatus::FAILED;
                    task.error_message = error;
                }
                break;
            }
        }
    }
    releaseDiskSpace(id);
    if (!owned) return;
    brls::Logger::error("DownloadManager: {}", error);
    saveState();
    if (!shuttingDown.load()) brls::sync([this, id]() { taskStatusChangedEvent.fire(id); });
    startNextTask();
}

bool DownloadManager::throttleBytes(size_t bytes, std::atomic<bool>& cancelFlag, std::atomic<bool>& pauseFlag) {
    if (bytes == 0) return true;
    std::unique_lock<std::mutex> lock(throttleMutex);
    while (!shuttingDown.load() && !cancelFlag.load() && !pauseFlag.load()) {
        const int64_t limit = effectiveSpeedLimit();
        const auto now = std::chrono::steady_clock::now();
        if (limit <= 0) {
            throttleTokens = 0.0;
            throttleLastRefill = now;
            return true;
        }

        const double elapsed = std::max(0.0, std::chrono::duration<double>(now - throttleLastRefill).count());
        // A quarter-second bucket keeps the configured TOTAL rate smooth while still tolerating
        // ordinary cpr callback chunk sizes. For very low limits, one chunk may be the capacity,
        // but tokens start/refill at the configured byte rate so it still waits the correct time.
        const double capacity = std::max<double>(static_cast<double>(bytes), static_cast<double>(limit) * 0.25);
        throttleTokens = std::min(capacity, throttleTokens + elapsed * static_cast<double>(limit));
        throttleLastRefill = now;
        if (throttleTokens >= static_cast<double>(bytes)) {
            throttleTokens -= static_cast<double>(bytes);
            return true;
        }

        const double secondsNeeded = (static_cast<double>(bytes) - throttleTokens) / static_cast<double>(limit);
        const auto waitFor = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(std::max(0.001, std::min(0.25, secondsNeeded))));
        throttleCv.wait_for(lock, waitFor, [&]() {
            return shuttingDown.load() || cancelFlag.load() || pauseFlag.load();
        });
    }
    return false;
}

bool DownloadManager::reserveDiskSpace(const std::string& taskId, const std::string& path,
                                       int64_t requiredBytes, int64_t& freeBytes) {
    std::lock_guard<std::mutex> lock(reservationMutex);
    freeBytes = availableBytes(path);
    if (requiredBytes <= 0 || freeBytes < 0) {
        diskReservations[taskId] = std::max<int64_t>(0, requiredBytes);
        return true;
    }
    int64_t reservedByOthers = 0;
    for (const auto& item : diskReservations) if (item.first != taskId) reservedByOthers += item.second;
    if (freeBytes - reservedByOthers < requiredBytes) return false;
    diskReservations[taskId] = requiredBytes;
    return true;
}

void DownloadManager::releaseDiskSpace(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(reservationMutex);
    diskReservations.erase(taskId);
}

void DownloadManager::updateLiveProgress(const std::string& taskId, int64_t downloaded, int64_t total, bool audioPart) {
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == taskId) {
            if (audioPart) { t.audio_downloaded_bytes = downloaded; t.audio_total_bytes = total; }
            else { t.downloaded_bytes = downloaded; t.total_bytes = total; }
            break;
        }
    }
    if (!shuttingDown.load()) brls::sync([this, taskId]() { taskProgressEvent.fire(taskId); });
}


void DownloadManager::updateStage(const std::string& taskId, DownloadTaskStage stage, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == taskId) {
            t.stage = stage;
            if (!error.empty()) t.error_message = error;
            break;
        }
    }
    if (!shuttingDown.load()) brls::sync([this, taskId]() { taskStatusChangedEvent.fire(taskId); });
}

bool DownloadManager::downloadFile(const std::string& url, const std::string& filepath,
                                   std::atomic<bool>& cancelFlag, std::atomic<bool>& pauseFlag,
                                   int64_t& downloadedBytes, int64_t& totalBytes,
                                   const std::string& taskId, bool audioPart) {
    int64_t existing = 0;
    try { if (cpr::fs::exists(filepath)) existing = static_cast<int64_t>(cpr::fs::file_size(filepath)); } catch (...) {}
    if (existing > 0 && totalBytes > 0 && existing >= totalBytes) {
        // A complete-sized file no longer needs its transport validator. Integrity is checked by
        // ffprobe before completion; if that fails verifyTaskMedia() quarantines it for redownload.
        try {
            const auto staleResumeMeta = filepath + ".resume.json";
            if (cpr::fs::exists(staleResumeMeta)) cpr::fs::remove(staleResumeMeta);
        } catch (...) {}
        downloadedBytes = existing;
        updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        return true;
    }

    const auto tempPath = filepath + ".request.part";
    const auto resumeMetaPath = filepath + ".resume.json";
    std::string resumeValidator;
    if (existing > 0 && cpr::fs::exists(resumeMetaPath)) {
        try {
            std::ifstream rf(resumeMetaPath);
            nlohmann::json resume; rf >> resume;
            resumeValidator = resume.value("validator", "");
        } catch (...) {}
    }
    std::ofstream ofs(tempPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;

    int64_t requestBytes = 0;
    downloadedBytes = existing;
    updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
    auto lastFire = std::chrono::steady_clock::now();

    auto session = bilibili::HTTP::createSession();
    session->SetUrl(cpr::Url{url});
    auto headers = bilibili::HTTP::HEADERS;
    if (existing > 0) {
        headers["Range"] = fmt::format("bytes={}-", existing);
        if (!resumeValidator.empty()) headers["If-Range"] = resumeValidator;
    }
    session->SetHeader(headers);
    session->SetTimeout(cpr::Timeout{0});
    session->SetWriteCallback(cpr::WriteCallback([&](std::string data, intptr_t) -> bool {
        if (cancelFlag.load() || pauseFlag.load() || shuttingDown.load()) return false;
        // One shared token bucket enforces the configured TOTAL background-download speed across
        // all concurrent workers. Sleeping inside the write callback naturally back-pressures cpr.
        if (!throttleBytes(data.size(), cancelFlag, pauseFlag)) return false;
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!ofs.good()) return false;
        requestBytes += static_cast<int64_t>(data.size());
        downloadedBytes = existing + requestBytes;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastFire).count() >= 0.25) {
            lastFire = now;
            updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        }
        return true;
    }));
    session->SetProgressCallback(cpr::ProgressCallback([&](cpr::cpr_pf_arg_t dltotal, cpr::cpr_pf_arg_t,
                                                     cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
        if (dltotal > 0) totalBytes = existing + static_cast<int64_t>(dltotal);
        return !cancelFlag.load() && !pauseFlag.load() && !shuttingDown.load();
    }));
    auto response = session->Get();
    ofs.close();

    std::string responseValidator = responseHeader(response, "etag");
    if (responseValidator.empty()) responseValidator = responseHeader(response, "last-modified");
    auto saveResumeValidator = [&](const std::string& validator) {
        try {
            if (validator.empty()) {
                if (cpr::fs::exists(resumeMetaPath)) cpr::fs::remove(resumeMetaPath);
                return;
            }
            std::ofstream meta(resumeMetaPath, std::ios::trunc);
            if (meta.is_open()) meta << nlohmann::json{{"validator", validator}}.dump();
        } catch (...) {}
    };
    auto clearResumeValidator = [&]() {
        try { if (cpr::fs::exists(resumeMetaPath)) cpr::fs::remove(resumeMetaPath); } catch (...) {}
    };

    const bool rangeResponse = response.status_code == 206;
    const bool rangeValid = rangeResponse && responseRangeStartsAt(response, existing);
    auto appendTemp = [&]() -> bool {
        if (!rangeValid) return false;
        std::ifstream in(tempPath, std::ios::binary);
        std::ofstream out(filepath, std::ios::binary | std::ios::app);
        if (!in.is_open() || !out.is_open()) return false;
        out << in.rdbuf();
        in.close(); out.close();
        if (!out.good()) return false;
        cpr::fs::remove(tempPath);
        downloadedBytes = existing + requestBytes;
        saveResumeValidator(!responseValidator.empty() ? responseValidator : resumeValidator);
        return true;
    };

    if (cancelFlag.load() || pauseFlag.load() || shuttingDown.load()) {
        try {
            if (requestBytes > 0) {
                if (rangeValid) {
                    appendTemp();
                } else if (existing == 0) {
                    if (!moveReplace(tempPath, filepath)) throw std::runtime_error("cannot preserve partial file");
                    downloadedBytes = requestBytes;
                    saveResumeValidator(responseValidator);
                } else {
                    // Never append an unvalidated 206 or an ignored-Range HTTP 200 to an existing
                    // partial. Keep the old verified prefix and discard only this unsafe chunk.
                    cpr::fs::remove(tempPath);
                    downloadedBytes = existing;
                }
            } else if (cpr::fs::exists(tempPath)) {
                cpr::fs::remove(tempPath);
            }
        } catch (const std::exception& e) {
            brls::Logger::error("DownloadManager: preserving partial file failed: {}", e.what());
        }
        updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        return false;
    }

    if (response.error || (response.status_code != 200 && response.status_code != 206)) {
        try {
            const bool mediaStatus = response.status_code == 200 || response.status_code == 206;
            if (requestBytes > 0 && mediaStatus) {
                if (rangeValid) {
                    appendTemp();
                } else if (response.status_code == 200 && existing == 0) {
                    // A transport error can still leave a useful prefix of a normal HTTP 200 body.
                    // Never persist HTTP error pages or an unvalidated 206 response as media.
                    if (moveReplace(tempPath, filepath)) {
                        downloadedBytes = requestBytes;
                        saveResumeValidator(responseValidator);
                    }
                } else if (cpr::fs::exists(tempPath)) {
                    cpr::fs::remove(tempPath);
                }
            } else if (cpr::fs::exists(tempPath)) {
                cpr::fs::remove(tempPath);
            }
        } catch (...) {}
        updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        brls::Logger::error("DownloadManager: media request to {} failed, HTTP {}, {}",
                            urlHost(url), response.status_code, response.error.message);
        return false;
    }

    // RFC-compliant 206 responses must describe the exact returned range. A CDN/source switch can
    // otherwise make a syntactically successful resume silently corrupt the media file.
    if (rangeResponse && !rangeValid) {
        brls::Logger::warning("DownloadManager: invalid Content-Range from {}; restarting track from byte 0", urlHost(url));
        try {
            if (cpr::fs::exists(tempPath)) cpr::fs::remove(tempPath);
            if (cpr::fs::exists(filepath)) cpr::fs::remove(filepath);
            clearResumeValidator();
        } catch (...) {}
        downloadedBytes = 0;
        totalBytes = 0;
        updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        return false;
    }

    try {
        if (rangeValid) {
            if (!appendTemp()) throw std::runtime_error("cannot append validated Range response");
        } else {
            // HTTP 200 after a Range request means the server ignored Range and sent a full body.
            // Atomically replace the old partial rather than duplicating bytes.
            if (!moveReplace(tempPath, filepath)) throw std::runtime_error("cannot finalize downloaded file");
            downloadedBytes = requestBytes;
            totalBytes = requestBytes;
        }
        clearResumeValidator();
    } catch (const std::exception& e) {
        brls::Logger::error("DownloadManager: finalizing file failed: {}", e.what());
        return false;
    }
    updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
    return true;
}

bool DownloadManager::refreshSource(DownloadTask& task) {
    if (task.cid == 0) return false;

    struct RefreshState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        bilibili::VideoUrlResult result;
        std::string error;
    };
    auto state = std::make_shared<RefreshState>();
    auto success = [state](const bilibili::VideoUrlResult& result) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = result;
            state->ok = true;
            state->done = true;
        }
        state->cv.notify_one();
    };
    auto failure = [state](BILI_ERR) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->error = error;
            state->done = true;
        }
        state->cv.notify_one();
    };

    if (task.is_pgc) {
        BILI::get_season_url(
            task.cid, task.quality,
            [success](const bilibili::SeasonUrlResult& result) mutable { success(result.video_info); }, failure);
    } else if (!task.bvid.empty()) {
        BILI::get_video_url(task.bvid, task.cid, task.quality, success, failure);
    } else {
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!state->done && !cancelledOrPaused(task) && !shuttingDown.load()) {
            if (state->cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        if (cancelledOrPaused(task) || shuttingDown.load()) return false;
        if (!state->done) {
            brls::Logger::warning("DownloadManager: source refresh timed out for {}", task.id);
            return false;
        }
        if (!state->ok) {
            brls::Logger::warning("DownloadManager: source refresh failed for {}: {}", task.id, state->error);
            return false;
        }
    }

    const auto& result = state->result;
    const bool expectedDash = task.is_dash;
    if (expectedDash) {
        if (result.dash.video.empty()) return false;
        std::vector<bilibili::DashMedia> videos;
        for (const auto& media : result.dash.video) if (media.id == task.quality) videos.emplace_back(media);
        // Batch items do not always expose exactly the same quality ladder. Bilibili normally
        // reports the actual fallback in result.quality; use it rather than failing the whole batch.
        if (videos.empty()) {
            for (const auto& media : result.dash.video) if (media.id == result.quality) videos.emplace_back(media);
        }
        if (videos.empty()) videos = result.dash.video;
        if (videos.empty()) return false;

        bilibili::DashMedia video = videos.front();
        for (const auto& media : videos) {
            if (media.codecid == task.video_codec_id && task.video_codec_id != 0) {
                video = media;
                break;
            }
            if (media.bandwidth > video.bandwidth) video = media;
        }
        task.video_urls = mediaUrls(video);
        task.quality = video.id;
        for (size_t i = 0; i < result.accept_quality.size() && i < result.accept_description.size(); ++i) {
            if (result.accept_quality[i] == task.quality) { task.quality_desc = result.accept_description[i]; break; }
        }
        task.video_codec_id = video.codecid;
        task.video_bandwidth = video.bandwidth;
        task.video_width = video.width;
        task.video_height = video.height;
        task.flv_segments.clear();

        if (task.audio_id != 0 || !task.audio_urls.empty()) {
            std::vector<bilibili::DashMedia> candidates = result.dash.audio;
            candidates.insert(candidates.end(), result.dash.dolby_audio.begin(), result.dash.dolby_audio.end());
            if (result.dash.has_flac) candidates.emplace_back(result.dash.flac_audio);
            auto it = std::find_if(candidates.begin(), candidates.end(), [&](const bilibili::DashMedia& media) {
                return media.id == task.audio_id &&
                       (task.audio_codec_id == 0 || media.codecid == task.audio_codec_id);
            });
            if (it == candidates.end()) {
                it = std::find_if(candidates.begin(), candidates.end(), [&](const bilibili::DashMedia& media) {
                    return media.id == task.audio_id;
                });
            }
            if (it == candidates.end() && !candidates.empty()) {
                it = std::max_element(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                    return a.bandwidth < b.bandwidth;
                });
            }
            if (it == candidates.end()) {
                task.audio_id = 0;
                task.audio_codec_id = 0;
                task.audio_bandwidth = 0;
                task.audio_urls.clear();
                task.audio_file.clear();
            } else {
                task.audio_id = it->id;
                task.audio_urls = mediaUrls(*it);
                task.audio_codec_id = it->codecid;
                task.audio_bandwidth = it->bandwidth;
            }
        }
        task.estimated_bytes = estimateBytes(task);
        return !task.video_urls.empty();
    }

    if (result.durl.empty()) return false;
    task.video_urls.clear();
    task.audio_urls.clear();
    task.flv_segments.clear();
    for (const auto& item : result.durl) {
        FlvSegment segment;
        segment.url = item.url;
        segment.backup_urls = item.backup_url;
        segment.size = item.size > 0 ? static_cast<uint64_t>(item.size) : 0;
        segment.order = item.order;
        task.flv_segments.emplace_back(std::move(segment));
    }
    task.estimated_bytes = estimateBytes(task);
    return true;
}

bool DownloadManager::downloadDash(DownloadTask& task) {
    cpr::fs::create_directories(task.dir);
    updateStage(task.id, DownloadTaskStage::DOWNLOADING_VIDEO);
    task.stage = DownloadTaskStage::DOWNLOADING_VIDEO;
    const auto videoPath = joinPath(task.dir, task.video_file);
    bool videoComplete = false;
    try {
        videoComplete = task.total_bytes > 0 && task.downloaded_bytes >= task.total_bytes &&
                        cpr::fs::exists(videoPath) &&
                        static_cast<int64_t>(cpr::fs::file_size(videoPath)) >= task.total_bytes;
    } catch (...) {}

    bool ok = videoComplete;
    if (!videoComplete) {
        for (const auto& url : task.video_urls) {
            if (downloadFile(url, videoPath, *task.cancelFlag, *task.pauseFlag,
                             task.downloaded_bytes, task.total_bytes, task.id, false)) { ok = true; break; }
            if (task.cancelFlag->load() || task.pauseFlag->load()) return false;
        }
    }
    if (!ok) return false;

    if (!task.audio_urls.empty()) {
        updateStage(task.id, DownloadTaskStage::DOWNLOADING_AUDIO);
        task.stage = DownloadTaskStage::DOWNLOADING_AUDIO;
        const auto audioPath = joinPath(task.dir, task.audio_file);
        bool audioComplete = false;
        try {
            audioComplete = task.audio_total_bytes > 0 && task.audio_downloaded_bytes >= task.audio_total_bytes &&
                            cpr::fs::exists(audioPath) &&
                            static_cast<int64_t>(cpr::fs::file_size(audioPath)) >= task.audio_total_bytes;
        } catch (...) {}
        ok = audioComplete;
        if (!audioComplete) {
            for (const auto& url : task.audio_urls) {
                if (downloadFile(url, audioPath, *task.cancelFlag, *task.pauseFlag,
                                 task.audio_downloaded_bytes, task.audio_total_bytes, task.id, true)) { ok = true; break; }
                if (task.cancelFlag->load() || task.pauseFlag->load()) return false;
            }
        }
        if (!ok) return false;
    }
    return true;
}


int DownloadManager::runTool(const std::vector<std::string>& args, const DownloadTask* task, std::string* output) {
    if (args.empty()) return -1;
    if (output) output->clear();
#if !defined(_WIN32) && !defined(__SWITCH__) && !defined(__PSV__) && !defined(PS4)
    int pipeFd[2] = {-1, -1};
    if (output && ::pipe(pipeFd) != 0) return -1;

    pid_t pid = ::fork();
    if (pid < 0) {
        if (pipeFd[0] >= 0) ::close(pipeFd[0]);
        if (pipeFd[1] >= 0) ::close(pipeFd[1]);
        return -1;
    }
    if (pid == 0) {
        if (output) {
            ::close(pipeFd[0]);
            if (::dup2(pipeFd[1], STDOUT_FILENO) < 0) ::_exit(126);
            ::close(pipeFd[1]);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    if (output) {
        ::close(pipeFd[1]);
        const int flags = ::fcntl(pipeFd[0], F_GETFL, 0);
        if (flags >= 0) ::fcntl(pipeFd[0], F_SETFL, flags | O_NONBLOCK);
    }

    auto drainOutput = [&]() {
        if (!output || pipeFd[0] < 0) return;
        char buffer[4096];
        while (true) {
            const ssize_t count = ::read(pipeFd[0], buffer, sizeof(buffer));
            if (count > 0) output->append(buffer, static_cast<size_t>(count));
            else if (count < 0 && errno == EINTR) continue;
            else break;
        }
    };

    int status = 0;
    while (true) {
        drainOutput();
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0) {
            if (errno == EINTR) continue;
            if (pipeFd[0] >= 0) ::close(pipeFd[0]);
            return -1;
        }
        if (shuttingDown.load() || (task && cancelledOrPaused(*task))) {
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 10; ++i) {
                drainOutput();
                if (::waitpid(pid, &status, WNOHANG) == pid) {
                    if (pipeFd[0] >= 0) { drainOutput(); ::close(pipeFd[0]); }
                    return -2;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            if (pipeFd[0] >= 0) { drainOutput(); ::close(pipeFd[0]); }
            return -2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (pipeFd[0] >= 0) { drainOutput(); ::close(pipeFd[0]); }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#elif defined(_WIN32)
    // Keep Windows compatible with the existing command-line path. AppImage/SteamOS uses the
    // cancellable fork/exec implementation above. Capture is used only for ffprobe verification.
    std::string command;
    for (const auto& arg : args) {
        if (!command.empty()) command += ' ';
        command += shellQuote(arg);
    }
    std::string capturePath;
    if (output) {
        capturePath = task && !task->dir.empty() ? joinPath(task->dir, ".wiliwili-tool-output.tmp")
                                                 : ".wiliwili-tool-output.tmp";
        command += " > " + shellQuote(capturePath) + " 2>nul";
    }
    const int rc = std::system(command.c_str());
    if (output && !capturePath.empty()) {
        try {
            std::ifstream f(capturePath, std::ios::binary);
            if (f.is_open()) output->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            if (cpr::fs::exists(capturePath)) cpr::fs::remove(capturePath);
        } catch (...) {}
    }
    return rc;
#else
    // Consoles do not ship the external ffmpeg/ffprobe tools, so this path is normally unreachable.
    std::string command;
    for (const auto& arg : args) {
        if (!command.empty()) command += ' ';
        command += shellQuote(arg);
    }
    return std::system(command.c_str());
#endif
}

bool DownloadManager::probeMediaFile(const std::string& path, const DownloadTask& task,
                                     bool requireVideo, bool requireAudio, bool checkTaskDuration) {
    try {
        if (path.empty() || !cpr::fs::exists(path) || cpr::fs::file_size(path) == 0) return false;
    } catch (...) { return false; }
    if (!hasFfprobe()) return true;

    std::string output;
    const int rc = runTool({"ffprobe", "-v", "error", "-show_entries",
                            "stream=codec_type,duration:format=duration", "-of", "json", path}, &task, &output);
    if (rc != 0 || output.empty()) return false;

    try {
        const auto data = nlohmann::json::parse(output);
        bool hasVideo = false;
        bool hasAudio = false;
        double duration = 0.0;
        auto readDuration = [&](const nlohmann::json& value) {
            try {
                double parsed = 0.0;
                if (value.is_number()) parsed = value.get<double>();
                else if (value.is_string() && !value.get<std::string>().empty()) parsed = std::stod(value.get<std::string>());
                if (parsed > duration) duration = parsed;
            } catch (...) {}
        };
        if (data.contains("streams") && data["streams"].is_array()) {
            for (const auto& stream : data["streams"]) {
                const auto type = stream.value("codec_type", "");
                if (type == "video") hasVideo = true;
                else if (type == "audio") hasAudio = true;
                if (stream.contains("duration")) readDuration(stream["duration"]);
            }
        }
        if (data.contains("format") && data["format"].is_object() && data["format"].contains("duration"))
            readDuration(data["format"]["duration"]);

        if (requireVideo && !hasVideo) return false;
        if (requireAudio && !hasAudio) return false;
        if (checkTaskDuration && task.duration_seconds > 0) {
            if (duration <= 0.0) return false;
            const double expected = static_cast<double>(task.duration_seconds);
            const double minimum = std::max(1.0, expected * 0.50);
            const double maximum = expected * 1.50 + 30.0;
            if (duration < minimum || duration > maximum) return false;
        }
        return true;
    } catch (const std::exception& e) {
        brls::Logger::warning("DownloadManager: cannot parse ffprobe output for {}: {}", path, e.what());
        return false;
    }
}

bool DownloadManager::verifyTaskMedia(DownloadTask& task) {
    updateStage(task.id, DownloadTaskStage::VERIFYING);
    task.stage = DownloadTaskStage::VERIFYING;

    auto resetVideo = [&]() {
        task.downloaded_bytes = 0;
        task.total_bytes = 0;
    };
    auto resetAudio = [&]() {
        task.audio_downloaded_bytes = 0;
        task.audio_total_bytes = 0;
    };

    if (task.muxed && !task.output_file.empty()) {
        const auto output = joinPath(task.dir, task.output_file);
        if (probeMediaFile(output, task, true, !task.audio_file.empty(), true)) return true;
        // Do not get stuck in an infinite Retry -> size-complete -> verify-fail loop. Keep the bad
        // container for inspection, reset progress, and let the next Retry resolve fresh URLs and
        // redownload the raw tracks.
        quarantineMediaFile(output);
        task.muxed = false;
        task.output_file.clear();
        resetVideo();
        resetAudio();
        return false;
    }

    if (task.is_dash) {
        bool valid = true;
        const auto videoPath = joinPath(task.dir, task.video_file);
        if (!probeMediaFile(videoPath, task, true, false, true)) {
            quarantineMediaFile(videoPath);
            resetVideo();
            valid = false;
        }
        if (!task.audio_file.empty()) {
            const auto audioPath = joinPath(task.dir, task.audio_file);
            if (!probeMediaFile(audioPath, task, false, true, true)) {
                quarantineMediaFile(audioPath);
                resetAudio();
                valid = false;
            }
        }
        return valid;
    }

    // Multi-segment FLV fallback uses a local M3U playlist while ffmpeg is unavailable. Validate every
    // controlled segment instead of asking ffprobe to interpret the playlist itself. Any invalid
    // segment is quarantined so Retry downloads just the missing/corrupt material again.
    if (task.video_file == "video.m3u" || task.video_file == "video.m3u8") {
        bool found = false;
        bool valid = true;
        try {
            for (const auto& entry : cpr::fs::directory_iterator(task.dir)) {
                if (!cpr::fs::is_regular_file(entry.path())) continue;
                const auto name = entry.path().filename().string();
                if (name.rfind("segment_", 0) == 0 && entry.path().extension() == ".flv") {
                    found = true;
                    if (!probeMediaFile(entry.path().string(), task, true, false, false)) {
                        quarantineMediaFile(entry.path().string());
                        valid = false;
                    }
                }
            }
            if (!valid) {
                const auto playlist = joinPath(task.dir, task.video_file);
                if (cpr::fs::exists(playlist)) cpr::fs::remove(playlist);
                task.output_file.clear();
                resetVideo();
            }
        } catch (...) {
            valid = false;
            resetVideo();
        }
        return found && valid;
    }

    const auto videoPath = joinPath(task.dir, task.video_file);
    if (probeMediaFile(videoPath, task, true, false, true)) return true;
    quarantineMediaFile(videoPath);
    task.output_file.clear();
    resetVideo();
    return false;
}

bool DownloadManager::concatFlvSegments(DownloadTask& task, const std::vector<std::string>& segmentFiles) {
    if (segmentFiles.empty()) return false;
    if (hasFfmpeg()) {
        const auto listPath = joinPath(task.dir, "segments.ffconcat");
        const auto outputPath = joinPath(task.dir, "video.flv");
        try {
            std::ofstream list(listPath, std::ios::trunc);
            if (!list.is_open()) return false;
            list << "ffconcat version 1.0\n";
            for (const auto& path : segmentFiles)
                list << "file '" << cpr::fs::path(path).filename().string() << "'\n";
            list.close();
        } catch (...) { return false; }

        const int rc = runTool({"ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                                "-f", "concat", "-safe", "0", "-i", listPath,
                                "-c", "copy", outputPath}, &task);
        try { if (cpr::fs::exists(listPath)) cpr::fs::remove(listPath); } catch (...) {}
        if (rc == 0 && probeMediaFile(outputPath, task, true, false, true)) {
            task.video_file = "video.flv";
            task.output_file = task.video_file;
            for (const auto& path : segmentFiles) { try { cpr::fs::remove(path); } catch (...) {} }
            return true;
        }
        try { if (cpr::fs::exists(outputPath)) cpr::fs::remove(outputPath); } catch (...) {}
        if (cancelledOrPaused(task) || shuttingDown.load()) return false;
    }

    // No ffmpeg (or concat failed): preserve independent valid FLV segments. A local M3U playlist lets mpv
    // play them sequentially without unsafe byte concatenation or duplicate FLV headers/timestamps.
    try {
        const auto playlist = joinPath(task.dir, "video.m3u");
        std::ofstream out(playlist, std::ios::trunc);
        if (!out.is_open()) return false;
        out << "#EXTM3U\n";
        for (const auto& path : segmentFiles) {
            out << "#EXTINF:-1,\n" << cpr::fs::path(path).filename().string() << "\n";
        }
        out.close();
        task.video_file = "video.m3u";
        task.output_file = task.video_file;
        return true;
    } catch (...) { return false; }
}

bool DownloadManager::tryMuxDash(DownloadTask& task) {
    if (!task.is_dash || task.audio_file.empty()) return false;
    updateStage(task.id, DownloadTaskStage::MUXING);
    task.stage = DownloadTaskStage::MUXING;
    const auto videoPath = joinPath(task.dir, task.video_file);
    const auto audioPath = joinPath(task.dir, task.audio_file);
    if (!cpr::fs::exists(videoPath) || !cpr::fs::exists(audioPath) || !hasFfmpeg()) return false;

    const auto outputPath = joinPath(task.dir, "video.mp4");
    const int rc = runTool({"ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                            "-i", videoPath, "-i", audioPath, "-map", "0:v:0", "-map", "1:a:0",
                            "-c", "copy", outputPath}, &task);
    if (rc == 0 && probeMediaFile(outputPath, task, true, true, true)) {
        task.output_file = "video.mp4";
        task.muxed = true;
        // Delete raw DASH tracks only after ffprobe has verified the final container.
        try { cpr::fs::remove(videoPath); } catch (...) {}
        try { cpr::fs::remove(audioPath); } catch (...) {}
        return true;
    }
    try { if (cpr::fs::exists(outputPath)) cpr::fs::remove(outputPath); } catch (...) {}
    return false;
}

void DownloadManager::writeReadme(const DownloadTask& task) {
    try {
        std::ofstream f(joinPath(task.dir, "README.txt"), std::ios::trunc);
        if (!f.is_open()) return;
        f << "wiliwili offline download\n\n";
        f << "Title: " << task.title << "\n";
        f << "Uploader: " << task.owner_name << "\n";
        f << "BVID: " << task.bvid << "\nCID: " << task.cid << "\nAID: " << task.aid << "\n";
        f << "Source page: " << task.source_page_url << "\n";
        f << "Video quality: " << task.quality_desc << " (" << task.quality << ")\n";
        f << "Video codec id: " << task.video_codec_id << "\n";
        f << "Audio: " << task.audio_desc << "\n\n";
        if (task.muxed && !task.output_file.empty()) {
            f << "Playable file: " << task.output_file << "\n";
        } else if (task.is_dash && !task.audio_file.empty()) {
            f << "Video track: " << task.video_file << "\nAudio track: " << task.audio_file << "\n";
            f << "The built-in offline player can play these tracks together. To create a single MP4:\n";
            f << "ffmpeg -i " << task.video_file << " -i " << task.audio_file << " -map 0:v:0 -map 1:a:0 -c copy video.mp4\n";
        } else {
            f << "Playable/source file: " << task.video_file << "\n";
        }
        f << "\nStable source metadata is in source.json. Temporary signed CDN URLs are not persisted by default; "
             "enable the debug-source setting to write debug_source.json.\n";
        f << "Metadata is in info.json; danmaku is danmaku.xml; available subtitles are exported as .srt and .ass.\n";
    } catch (...) {}
}

bool DownloadManager::downloadFlv(DownloadTask& task) {
    cpr::fs::create_directories(task.dir);
    updateStage(task.id, DownloadTaskStage::DOWNLOADING_VIDEO);
    task.stage = DownloadTaskStage::DOWNLOADING_VIDEO;
    if (task.flv_segments.empty() && !task.video_urls.empty()) {
        return downloadFile(task.video_urls.front(), joinPath(task.dir, task.video_file), *task.cancelFlag, *task.pauseFlag,
                            task.downloaded_bytes, task.total_bytes, task.id, false);
    }
    if (task.flv_segments.size() == 1) {
        const auto path = joinPath(task.dir, task.video_file);
        const auto expected = static_cast<int64_t>(task.flv_segments[0].size);
        try {
            if (expected > 0 && cpr::fs::exists(path) && static_cast<int64_t>(cpr::fs::file_size(path)) >= expected) {
                task.downloaded_bytes = expected;
                task.total_bytes = expected;
                updateLiveProgress(task.id, expected, expected, false);
                return true;
            }
        } catch (...) {}
        std::vector<std::string> urls{task.flv_segments[0].url};
        urls.insert(urls.end(), task.flv_segments[0].backup_urls.begin(), task.flv_segments[0].backup_urls.end());
        for (const auto& url : urls) {
            if (downloadFile(url, path, *task.cancelFlag, *task.pauseFlag,
                             task.downloaded_bytes, task.total_bytes, task.id, false)) return true;
            if (task.cancelFlag->load() || task.pauseFlag->load()) return false;
        }
        return false;
    }

    std::vector<std::string> segFiles;
    int64_t completedBytes = 0;
    int64_t expectedTotal = 0;
    for (const auto& seg : task.flv_segments) expectedTotal += static_cast<int64_t>(seg.size);
    for (size_t i = 0; i < task.flv_segments.size(); ++i) {
        auto path = joinPath(task.dir, fmt::format("segment_{:03}.flv", i));
        segFiles.push_back(path);
        int64_t dl = 0, total = 0;
        bool ok = false;
        const auto expected = static_cast<int64_t>(task.flv_segments[i].size);
        try {
            if (expected > 0 && cpr::fs::exists(path) && static_cast<int64_t>(cpr::fs::file_size(path)) >= expected) {
                dl = expected;
                total = expected;
                ok = true;
            }
        } catch (...) {}
        if (!ok) {
            std::vector<std::string> urls{task.flv_segments[i].url};
            urls.insert(urls.end(), task.flv_segments[i].backup_urls.begin(), task.flv_segments[i].backup_urls.end());
            for (const auto& url : urls) {
                if (downloadFile(url, path, *task.cancelFlag, *task.pauseFlag, dl, total, task.id, false)) { ok = true; break; }
                if (task.cancelFlag->load() || task.pauseFlag->load()) return false;
            }
        }
        if (!ok) return false;
        completedBytes += dl;
        task.downloaded_bytes = completedBytes;
        task.total_bytes = expectedTotal > 0 ? expectedTotal : 0;
        updateLiveProgress(task.id, task.downloaded_bytes, task.total_bytes, false);
    }

    if (!concatFlvSegments(task, segFiles)) return false;
    task.total_bytes = task.downloaded_bytes;
    updateLiveProgress(task.id, task.downloaded_bytes, task.total_bytes, false);
    return true;
}

void DownloadManager::saveMetadata(const DownloadTask& task, bool completed) {
    try {
        cpr::fs::create_directories(task.dir);
        auto& appVersion = APPVersion::instance();
        nlohmann::json source = {
            {"schema_version", 3}, {"app", "wiliwili"}, {"app_version", appVersion.getVersionStr()},
            {"app_git_commit", appVersion.git_commit}, {"source_page_url", task.source_page_url}, {"title", task.title},
            {"series_title", task.series_title}, {"part_title", task.part_title}, {"part_index", task.part_index},
            {"owner_name", task.owner_name}, {"cover_url", task.cover_url}, {"bvid", task.bvid}, {"aid", task.aid},
            {"cid", task.cid}, {"is_pgc", task.is_pgc}, {"format", task.is_dash ? "dash" : "flv"},
            {"duration_seconds", task.duration_seconds}, {"download_root", task.download_root},
            {"created_at", task.created_at}, {"finished_at", task.finished_at},
            {"video", {{"quality_id", task.quality}, {"quality_desc", task.quality_desc}, {"codec_id", task.video_codec_id},
                       {"bandwidth", task.video_bandwidth}, {"width", task.video_width}, {"height", task.video_height},
                       {"file", task.video_file}}},
            {"audio", {{"id", task.audio_id}, {"desc", task.audio_desc}, {"codec_id", task.audio_codec_id},
                       {"bandwidth", task.audio_bandwidth}, {"file", task.audio_file}}},
            {"output_file", task.output_file}, {"muxed", task.muxed}
        };
        if (!writeJsonAtomic(joinPath(task.dir, "source.json"), source))
            brls::Logger::warning("DownloadManager: could not atomically write source.json for {}", task.id);

        // Signed CDN URLs are sensitive, short-lived diagnostics. They are off by default and are
        // never stored in download_state.json. Users can explicitly enable this file for debugging.
        const auto debugPath = joinPath(task.dir, "debug_source.json");
        if (runtimeDebugSource.load()) {
            nlohmann::json debugSource = {
                {"video_urls", task.video_urls}, {"audio_urls", task.audio_urls}, {"flv_segments", task.flv_segments}
            };
            if (!writeJsonAtomic(debugPath, debugSource))
                brls::Logger::warning("DownloadManager: could not atomically write debug_source.json for {}", task.id);
        } else {
            try { if (cpr::fs::exists(debugPath)) cpr::fs::remove(debugPath); } catch (...) {}
        }

        // Marker used by deleteTask(). Destructive recursive deletion is allowed only when this
        // marker (or legacy matching info.json) proves that the directory belongs to this task.
        nlohmann::json marker = {{"schema_version", 2}, {"app", "wiliwili"}, {"task_id", task.id},
                                 {"bvid", task.bvid}, {"cid", task.cid}, {"download_root", task.download_root}};
        if (!writeJsonAtomic(joinPath(task.dir, ".wiliwili-download.json"), marker))
            brls::Logger::warning("DownloadManager: could not atomically write ownership marker for {}", task.id);

        std::vector<std::string> subtitleFiles;
        try {
            for (const auto& entry : cpr::fs::directory_iterator(task.dir)) {
                if (!cpr::fs::is_regular_file(entry.path())) continue;
                const auto ext = entry.path().extension().string();
                if (ext == ".srt" || ext == ".ass") subtitleFiles.emplace_back(entry.path().filename().string());
            }
            std::sort(subtitleFiles.begin(), subtitleFiles.end());
        } catch (...) {}

        nlohmann::json info = {
            {"schema_version", 3}, {"id", task.id}, {"title", task.title}, {"series_title", task.series_title},
            {"part_title", task.part_title}, {"part_index", task.part_index}, {"owner_name", task.owner_name},
            {"cover_url", task.cover_url}, {"bvid", task.bvid}, {"aid", task.aid}, {"cid", task.cid},
            {"quality", task.quality}, {"quality_desc", task.quality_desc}, {"audio_id", task.audio_id},
            {"audio_desc", task.audio_desc}, {"is_dash", task.is_dash}, {"is_pgc", task.is_pgc},
            {"duration_seconds", task.duration_seconds}, {"estimated_bytes", estimateBytes(task)},
            {"video_file", task.video_file}, {"audio_file", task.audio_file}, {"output_file", task.output_file},
            {"downloaded_bytes", task.downloaded_bytes}, {"total_bytes", task.total_bytes},
            {"audio_downloaded_bytes", task.audio_downloaded_bytes}, {"audio_total_bytes", task.audio_total_bytes},
            {"download_root", task.download_root}, {"download_dir", task.dir}, {"muxed", task.muxed},
            {"created_at", task.created_at},
            {"finished_at", task.finished_at}, {"completed", completed}, {"source_file", "source.json"},
            {"debug_source_file", runtimeDebugSource.load() ? "debug_source.json" : ""}, {"cover_file", "cover.jpg"}, {"danmaku_file", "danmaku.xml"},
            {"subtitle_files", subtitleFiles}
        };
        if (!writeJsonAtomic(joinPath(task.dir, "info.json"), info))
            brls::Logger::warning("DownloadManager: could not atomically write info.json for {}", task.id);
    } catch (const std::exception& e) { brls::Logger::error("DownloadManager: metadata failed: {}", e.what()); }
}

void DownloadManager::saveDanmaku(const DownloadTask& task) {
    if (!task.cid || task.dir.empty()) return;
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<State>();
    const auto dir = task.dir;
    auto cancel = task.cancelFlag;
    auto pause = task.pauseFlag;
    BILI::get_danmaku(task.cid, [dir, state, cancel, pause](const std::string& xml) {
        if ((!cancel || !cancel->load()) && (!pause || !pause->load())) {
            try {
                std::ofstream f(joinPath(dir, "danmaku.xml"), std::ios::trunc);
                if (f.is_open()) f << xml;
            } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->cv.notify_one();
    }, [state](BILI_ERR) {
        brls::Logger::warning("DownloadManager: danmaku failed: {}", error);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(state->mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!state->done && !cancelledOrPaused(task) && !shuttingDown.load()) {
        if (state->cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
}

void DownloadManager::saveSubtitles(const DownloadTask& task) {
    if (!task.cid || task.dir.empty()) return;

    struct PageState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        bilibili::VideoPageResult page;
    };
    auto pageState = std::make_shared<PageState>();
    auto onPage = [pageState](const bilibili::VideoPageResult& pageResult) {
        {
            std::lock_guard<std::mutex> lock(pageState->mutex);
            pageState->page = pageResult;
            pageState->ok = true;
            pageState->done = true;
        }
        pageState->cv.notify_one();
    };
    auto onPageError = [pageState](BILI_ERR) {
        brls::Logger::warning("DownloadManager: page detail for subtitles failed: {}", error);
        {
            std::lock_guard<std::mutex> lock(pageState->mutex);
            pageState->done = true;
        }
        pageState->cv.notify_one();
    };
    if (!task.bvid.empty()) BILI::get_page_detail(task.bvid, task.cid, onPage, onPageError);
    else if (task.aid != 0) BILI::get_page_detail(task.aid, task.cid, onPage, onPageError);
    else return;

    bilibili::VideoPageResult page;
    {
        std::unique_lock<std::mutex> lock(pageState->mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (!pageState->done && !cancelledOrPaused(task) && !shuttingDown.load()) {
            if (pageState->cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        if (!pageState->done || !pageState->ok || cancelledOrPaused(task) || shuttingDown.load()) return;
        page = pageState->page;
    }
    if (page.subtitles.empty()) return;

    struct SubtitleState {
        std::mutex mutex;
        std::condition_variable cv;
        size_t remaining = 0;
    };
    auto subtitleState = std::make_shared<SubtitleState>();
    for (const auto& subtitle : page.subtitles) if (!subtitle.subtitle_url.empty()) ++subtitleState->remaining;
    if (subtitleState->remaining == 0) return;

    const auto dir = task.dir;
    auto cancel = task.cancelFlag;
    auto pause = task.pauseFlag;
    auto finishOne = [subtitleState]() {
        {
            std::lock_guard<std::mutex> lock(subtitleState->mutex);
            if (subtitleState->remaining > 0) --subtitleState->remaining;
        }
        subtitleState->cv.notify_one();
    };

    for (const auto& subtitle : page.subtitles) {
        if (subtitle.subtitle_url.empty()) continue;
        BILI::get_subtitle(subtitle.subtitle_url,
            [dir, subtitle, finishOne, cancel, pause](const bilibili::SubtitleData& data) {
                if ((!cancel || !cancel->load()) && (!pause || !pause->load()) && cpr::fs::exists(dir)) {
                    const auto base = subtitleSafeName(subtitle.lan.empty() ? subtitle.id_str : subtitle.lan);
                    const auto srtPath = joinPath(dir, "subtitle." + base + ".srt");
                    const auto assPath = joinPath(dir, "subtitle." + base + ".ass");
                    try {
                        std::ofstream srt(srtPath, std::ios::trunc);
                        if (srt.is_open()) {
                            for (size_t i = 0; i < data.body.size(); ++i) {
                                const auto& line = data.body[i];
                                srt << (i + 1) << "\n" << srtTime(line.from) << " --> " << srtTime(line.to) << "\n"
                                    << line.content << "\n\n";
                            }
                        }
                        std::ofstream ass(assPath, std::ios::trunc);
                        if (ass.is_open()) {
                            ass << "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n"
                                   "[V4+ Styles]\nFormat: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,MarginL,MarginR,MarginV,Encoding\n"
                                   "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,2,0,2,40,40,35,1\n"
                                   "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n";
                            for (const auto& line : data.body) {
                                ass << "Dialogue: 0," << assTime(line.from) << ',' << assTime(line.to)
                                    << ",Default,,0,0,0,," << escapeAss(line.content) << "\n";
                            }
                        }
                    } catch (...) {}
                }
                finishOne();
            },
            [finishOne](BILI_ERR) {
                brls::Logger::warning("DownloadManager: subtitle failed: {}", error);
                finishOne();
            });
    }

    std::unique_lock<std::mutex> lock(subtitleState->mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (subtitleState->remaining != 0 && !cancelledOrPaused(task) && !shuttingDown.load()) {
        if (subtitleState->cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
}


void DownloadManager::runTask(const std::string& id) {
    DownloadTask work;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& t){ return t.id == id; });
        if (it == tasks.end()) {
            runningTasks.erase(id);
            releaseDiskSpace(id);
            return;
        }
        work = *it;
    }

    cpr::fs::create_directories(work.dir);
    bool success = false;
    bool fatalSpaceError = false;
    std::string lastError;

    auto interruptibleBackoff = [&](int seconds) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancelledOrPaused(work) || shuttingDown.load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    };

    // Suspend/resume and Wi-Fi roaming can invalidate an in-flight CDN connection. Retry with a
    // freshly resolved play-url while keeping only Range-validated partial prefixes.
    for (int attempt = 0; attempt < 6 && !success; ++attempt) {
        if (cancelledOrPaused(work) || shuttingDown.load()) break;

        updateStage(work.id, DownloadTaskStage::REFRESHING_SOURCE);
        work.stage = DownloadTaskStage::REFRESHING_SOURCE;
        const bool refreshed = refreshSource(work);
        const bool hasStoredSource = work.is_dash ? !work.video_urls.empty()
                                                 : (!work.flv_segments.empty() || !work.video_urls.empty());
        if (!refreshed && !hasStoredSource) {
            lastError = "Unable to refresh the Bilibili media source";
            if (attempt < 5) {
                static const int delays[] = {2, 5, 10, 15, 20};
                if (!interruptibleBackoff(delays[attempt])) break;
                continue;
            }
            break;
        }

        work.estimated_bytes = estimateBytes(work);
        saveMetadata(work, false);

        updateStage(work.id, DownloadTaskStage::CHECKING_SPACE);
        work.stage = DownloadTaskStage::CHECKING_SPACE;
        const int64_t expected = estimateBytes(work);
        const int64_t already = work.downloaded_bytes + work.audio_downloaded_bytes;
        const int64_t remaining = expected > already ? expected - already : 0;
        constexpr int64_t SAFETY_MARGIN = 128LL * 1024LL * 1024LL;
        const int64_t muxReserve = work.is_dash && !work.audio_file.empty() && hasFfmpeg() ? expected : 0;
        const int64_t requiredFree = remaining + muxReserve + SAFETY_MARGIN;
        int64_t freeBytes = -1;
        if (!reserveDiskSpace(work.id, work.dir, requiredFree, freeBytes)) {
            int64_t reservedByOthers = 0;
            {
                std::lock_guard<std::mutex> lock(reservationMutex);
                for (const auto& item : diskReservations) if (item.first != work.id) reservedByOthers += item.second;
            }
            lastError = fmt::format(
                "Not enough free space: need about {:.1f} MB (+ {:.1f} MB reserved by other downloads), only {:.1f} MB available",
                requiredFree / 1048576.0, reservedByOthers / 1048576.0, freeBytes / 1048576.0);
            fatalSpaceError = true;
            break;
        }

        if (runtimeDownloadCover.load() && !work.cover_url.empty() && !cancelledOrPaused(work)) {
            const auto coverPath = joinPath(work.dir, "cover.jpg");
            if (!cpr::fs::exists(coverPath)) {
                try {
                    auto session = bilibili::HTTP::createSession();
                    session->SetUrl(cpr::Url{work.cover_url});
                    session->SetTimeout(cpr::Timeout{10000});
                    session->SetProgressCallback(cpr::ProgressCallback([&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                                                       cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
                        return !cancelledOrPaused(work) && !shuttingDown.load();
                    }));
                    auto response = session->Get();
                    if (!response.error && response.status_code == 200 && !cancelledOrPaused(work)) {
                        std::ofstream f(coverPath, std::ios::binary);
                        if (f.is_open()) f.write(response.text.data(), static_cast<std::streamsize>(response.text.size()));
                    }
                } catch (...) {}
            }
        }

        success = work.is_dash ? downloadDash(work) : downloadFlv(work);
        if (!success && !cancelledOrPaused(work) && !shuttingDown.load()) {
            lastError = "Download interrupted; the validated partial file was kept for resume";
            if (attempt < 5) {
                static const int delays[] = {2, 5, 10, 15, 20};
                if (!interruptibleBackoff(delays[attempt])) break;
            }
        }
    }

    if (success && !cancelledOrPaused(work) && !shuttingDown.load() && work.is_dash) {
        if (!work.audio_file.empty()) {
            if (!tryMuxDash(work)) {
                if (cancelledOrPaused(work) || shuttingDown.load()) {
                    success = false;
                } else {
                    // Raw DASH tracks remain a valid offline download if muxing fails/unavailable.
                    work.output_file = work.video_file;
                    if (hasFfmpeg()) lastError = "Downloaded successfully, but ffmpeg muxing failed; raw DASH tracks were kept";
                }
            }
        } else {
            work.output_file = work.video_file;
        }
    }
    if (success && !work.is_dash && work.output_file.empty()) work.output_file = work.video_file;

    if (success && !cancelledOrPaused(work) && !shuttingDown.load()) {
        if (!verifyTaskMedia(work)) {
            if (cancelledOrPaused(work) || shuttingDown.load()) {
                success = false;
            } else {
                lastError = "Media integrity verification failed; invalid media was kept as .corrupt and Retry will redownload it";
                success = false;
            }
        }
    }

    bool completed = false;
    bool staleWorker = false;
    std::string title;

    if (success && !cancelledOrPaused(work) && !shuttingDown.load()) {
        work.stage = DownloadTaskStage::FETCHING_EXTRAS;
        updateStage(work.id, DownloadTaskStage::FETCHING_EXTRAS);
        saveMetadata(work, false);
        writeReadme(work);
        if (runtimeDownloadDanmaku.load()) saveDanmaku(work);
        if (runtimeDownloadSubtitles.load() && !cancelledOrPaused(work)) saveSubtitles(work);
        if (!cancelledOrPaused(work) && !shuttingDown.load()) {
            work.finished_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        } else {
            success = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == id) {
            staleWorker = t.cancelFlag != work.cancelFlag || t.pauseFlag != work.pauseFlag;
            if (staleWorker) break;
            t.downloaded_bytes = work.downloaded_bytes; t.total_bytes = work.total_bytes;
            t.audio_downloaded_bytes = work.audio_downloaded_bytes; t.audio_total_bytes = work.audio_total_bytes;
            // Do not retain expiring signed CDN URLs in the long-lived task model.
            t.video_urls.clear(); t.audio_urls.clear(); t.flv_segments.clear();
            t.video_codec_id = work.video_codec_id; t.video_bandwidth = work.video_bandwidth;
            t.video_width = work.video_width; t.video_height = work.video_height;
            t.audio_codec_id = work.audio_codec_id; t.audio_bandwidth = work.audio_bandwidth;
            t.estimated_bytes = work.estimated_bytes; t.output_file = work.output_file; t.video_file = work.video_file;
            t.audio_file = work.audio_file; t.muxed = work.muxed; t.finished_at = work.finished_at; t.stage = work.stage;
            if (work.cancelFlag->load()) t.status = DownloadTaskStatus::CANCELLED;
            else if (work.pauseFlag->load() || shuttingDown.load()) t.status = DownloadTaskStatus::PAUSED;
            else if (success) {
                t.status = DownloadTaskStatus::COMPLETED;
                t.stage = DownloadTaskStage::COMPLETED;
                t.error_message = lastError;
                completed = true;
                title = t.title;
            } else {
                t.status = DownloadTaskStatus::FAILED;
                t.error_message = fatalSpaceError ? lastError : (lastError.empty() ? "Download failed" : lastError);
            }
            work.status = t.status;
            work.stage = t.stage;
            work.error_message = t.error_message;
            break;
        }
        runningTasks.erase(id);
    }

    releaseDiskSpace(id);
    if (completed) {
        saveMetadata(work, true);
        writeReadme(work);
    } else if (!staleWorker) {
        saveMetadata(work, false);
    }

    saveState();
    if (!shuttingDown.load()) {
        brls::sync([this, id, completed, title, staleWorker]() {
            if (!staleWorker) taskStatusChangedEvent.fire(id);
            if (completed) brls::Application::notify("wiliwili/player/download/completed"_i18n +
                                                     (title.empty() ? "" : ": " + title));
        });
    }
    startNextTask();
}
