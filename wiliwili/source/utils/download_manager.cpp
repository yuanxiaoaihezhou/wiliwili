#include "utils/download_manager.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <iomanip>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <cpr/cpr.h>
#include <cpr/filesystem.h>
#include <fmt/format.h>

#include "api/bilibili/util/http.hpp"
#include "api/bilibili/util/uuid.hpp"
#include "bilibili.h"
#include "utils/config_helper.hpp"

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
    try {
        if (cpr::fs::exists(to)) cpr::fs::remove(to);
    } catch (...) {
        return false;
    }
    return std::rename(from.c_str(), to.c_str()) == 0;
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

static bool hasFfmpeg() {
#if defined(__SWITCH__) || defined(__PSV__) || defined(PS4)
    return false;
#elif defined(_WIN32)
    return std::system("where ffmpeg >nul 2>nul") == 0;
#else
    return std::system("command -v ffmpeg >/dev/null 2>&1") == 0;
#endif
}

static std::string makeTaskId(const DownloadTask& task) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return bilibili::genUUID(task.bvid + std::to_string(task.cid) + std::to_string(now));
}

DownloadManager::DownloadManager() = default;
DownloadManager::~DownloadManager() {
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& task : tasks) if (task.cancelFlag) task.cancelFlag->store(true);
    }
    if (runtimeHooksInitialized) MPV_E->unsubscribe(mpvEventSubscription);
}

void DownloadManager::initRuntimeHooks() {
    if (runtimeHooksInitialized) return;
    runtimeHooksInitialized = true;
    mpvEventSubscription = MPV_E->subscribe([this](MpvEventEnum event) {
        if (event == MPV_RESUME || event == MPV_LOADED || event == START_FILE) playbackActive.store(true);
        else if (event == MPV_PAUSE || event == MPV_IDLE || event == MPV_STOP || event == END_OF_FILE || event == MPV_FILE_ERROR)
            playbackActive.store(false);
    });
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
    const auto path = joinPath(ProgramConfig::instance().getConfigDir(), "download_state.json");
    if (!cpr::fs::exists(path)) return;
    try {
        std::ifstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j; f >> j;
        std::lock_guard<std::mutex> lock(tasksMutex);
        tasks = j.get<std::vector<DownloadTask>>();
        for (auto& t : tasks) {
            if (t.status == DownloadTaskStatus::DOWNLOADING || t.status == DownloadTaskStatus::PENDING)
                t.status = DownloadTaskStatus::PAUSED;
            t.cancelFlag = std::make_shared<std::atomic<bool>>(false);
            t.pauseFlag = std::make_shared<std::atomic<bool>>(false);
        }
        brls::Logger::info("DownloadManager: loaded {} task(s)", tasks.size());
    } catch (const std::exception& e) {
        brls::Logger::error("DownloadManager: load state failed: {}", e.what());
    }
}

void DownloadManager::saveState() {
    try {
        const auto dir = ProgramConfig::instance().getConfigDir();
        cpr::fs::create_directories(dir);
        // Two downloads may finish at nearly the same time. Serialize snapshot+write as one
        // operation so an older snapshot can never overwrite a newer one.
        std::lock_guard<std::mutex> stateLock(stateFileMutex);
        std::vector<DownloadTask> snapshot;
        {
            std::lock_guard<std::mutex> lock(tasksMutex);
            snapshot = tasks;
        }
        const auto statePath = joinPath(dir, "download_state.json");
        const auto tempPath = statePath + ".tmp";
        std::ofstream f(tempPath, std::ios::trunc);
        if (!f.is_open()) return;
        f << nlohmann::json(snapshot).dump(2);
        f.close();
        if (!moveReplace(tempPath, statePath))
            brls::Logger::error("DownloadManager: cannot replace state file");
    } catch (const std::exception& e) {
        brls::Logger::error("DownloadManager: save state failed: {}", e.what());
    }
}

void DownloadManager::addTask(DownloadTask task) {
    if (task.id.empty()) task.id = makeTaskId(task);
    if (task.created_at == 0) task.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    task.schema_version = 2;
    if (task.series_title.empty()) task.series_title = task.title.empty() ? (task.bvid.empty() ? "Bilibili" : task.bvid) : task.title;
    if (task.part_title.empty()) task.part_title = task.title;
    if (task.estimated_bytes <= 0) task.estimated_bytes = estimateBytes(task);
    if (task.dir.empty()) {
        const auto base = defaultDownloadDir();
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
        task.schema_version = 2;
        if (task.series_title.empty()) task.series_title = task.title.empty() ? (task.bvid.empty() ? "Bilibili" : task.bvid) : task.title;
        if (task.part_title.empty()) task.part_title = task.title;
        if (task.estimated_bytes <= 0) task.estimated_bytes = estimateBytes(task);
        if (task.dir.empty()) {
            const auto base = defaultDownloadDir();
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

void DownloadManager::deleteTask(const std::string& id) {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& t) { return t.id == id; });
        if (it == tasks.end() || it->status == DownloadTaskStatus::DOWNLOADING || runningTasks.count(id) != 0) return;
        dir = it->dir;
        tasks.erase(it);
    }
    if (!dir.empty()) {
        try { if (cpr::fs::exists(dir)) cpr::fs::remove_all(dir); } catch (...) {}
    }
    saveState(); taskStatusChangedEvent.fire(id); startNextTask();
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

int DownloadManager::maxConcurrent() const {
    int value = 2;
    try {
        value = std::stoi(ProgramConfig::instance().getSettingItem(SettingItem::DOWNLOAD_CONCURRENCY, std::string{"2"}));
    } catch (...) {}
    return std::max(1, std::min(4, value));
}

int64_t DownloadManager::effectiveSpeedLimit() const {
    auto parse = [](const std::string& value) -> int64_t {
        try { const double mb = std::stod(value); return mb > 0 ? static_cast<int64_t>(mb * 1024.0 * 1024.0) : 0; }
        catch (...) { return 0; }
    };
    auto& conf = ProgramConfig::instance();
    int64_t normal = parse(conf.getSettingItem(SettingItem::DOWNLOAD_SPEED_LIMIT, std::string{"0"}));
    if (!playbackActive.load()) return normal;
    int64_t duringPlayback = parse(conf.getSettingItem(SettingItem::DOWNLOAD_PLAYBACK_SPEED_LIMIT, std::string{"5"}));
    if (duringPlayback <= 0) return normal;
    if (normal <= 0) return duringPlayback;
    return std::min(normal, duringPlayback);
}

void DownloadManager::startNextTask() {
    std::vector<std::string> starts;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        int active = static_cast<int>(runningTasks.size());
        const int limit = maxConcurrent();
        for (auto& t : tasks) {
            if (active >= limit) break;
            if (t.status == DownloadTaskStatus::PENDING && runningTasks.count(t.id) == 0) {
                t.status = DownloadTaskStatus::DOWNLOADING;
                t.stage = DownloadTaskStage::REFRESHING_SOURCE;
                t.error_message.clear();
                runningTasks.insert(t.id);
                starts.push_back(t.id);
                ++active;
            }
        }
    }
    if (!starts.empty()) saveState();
    for (const auto& id : starts) {
        std::thread([this, id]() {
            try {
                runTask(id);
            } catch (const std::exception& e) {
                const std::string error = std::string("Download worker error: ") + e.what();
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
                if (owned) {
                    brls::Logger::error("DownloadManager: {}", error);
                    saveState();
                    brls::sync([this, id]() { taskStatusChangedEvent.fire(id); });
                    startNextTask();
                }
            } catch (...) {
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
                                task.error_message = "Unexpected download worker error";
                            }
                            break;
                        }
                    }
                }
                if (owned) {
                    brls::Logger::error("DownloadManager: unexpected worker exception for {}", id);
                    saveState();
                    brls::sync([this, id]() { taskStatusChangedEvent.fire(id); });
                    startNextTask();
                }
            }
        }).detach();
    }
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
    brls::sync([this, taskId]() { taskProgressEvent.fire(taskId); });
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
    brls::sync([this, taskId]() { taskStatusChangedEvent.fire(taskId); });
}

bool DownloadManager::downloadFile(const std::string& url, const std::string& filepath,
                                   std::atomic<bool>& cancelFlag, std::atomic<bool>& pauseFlag,
                                   int64_t& downloadedBytes, int64_t& totalBytes,
                                   const std::string& taskId, bool audioPart) {
    int64_t existing = 0;
    try { if (cpr::fs::exists(filepath)) existing = static_cast<int64_t>(cpr::fs::file_size(filepath)); } catch (...) {}
    // A resumed task may already have finished this track before it was paused while downloading
    // the next track. Do not issue a Range request starting exactly at EOF (many CDNs answer 416).
    if (existing > 0 && totalBytes > 0 && existing >= totalBytes) {
        downloadedBytes = existing;
        updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        return true;
    }
    const auto tempPath = filepath + ".request.part";
    std::ofstream ofs(tempPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;

    int64_t requestBytes = 0;
    downloadedBytes = existing;
    updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);

    int64_t limit = effectiveSpeedLimit();
    auto windowStart = std::chrono::steady_clock::now();
    int64_t windowBytes = 0;
    auto lastFire = std::chrono::steady_clock::now();

    auto session = bilibili::HTTP::createSession();
    session->SetUrl(cpr::Url{url});
    auto headers = bilibili::HTTP::HEADERS;
    if (existing > 0) headers["Range"] = fmt::format("bytes={}-", existing);
    session->SetHeader(headers);
    session->SetTimeout(cpr::Timeout{0});
    session->SetWriteCallback(cpr::WriteCallback([&](std::string data, intptr_t) -> bool {
        if (cancelFlag.load() || pauseFlag.load()) return false;
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!ofs.good()) return false;
        requestBytes += static_cast<int64_t>(data.size());
        downloadedBytes = existing + requestBytes;
        if (limit > 0) {
            windowBytes += static_cast<int64_t>(data.size());
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - windowStart).count();
            if (elapsed < 1.0 && windowBytes >= limit) {
                auto ms = static_cast<int64_t>((1.0 - elapsed) * 1000.0);
                if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                windowStart = std::chrono::steady_clock::now(); windowBytes = 0;
            } else if (elapsed >= 1.0) {
                windowStart = now;
                windowBytes = 0;
                limit = effectiveSpeedLimit();
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastFire).count() >= 0.25) {
            lastFire = now; updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        }
        return true;
    }));
    session->SetProgressCallback(cpr::ProgressCallback([&](cpr::cpr_pf_arg_t dltotal, cpr::cpr_pf_arg_t,
                                                     cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
        if (dltotal > 0) totalBytes = existing + static_cast<int64_t>(dltotal);
        return !cancelFlag.load() && !pauseFlag.load();
    }));
    auto response = session->Get();
    ofs.close();

    if (cancelFlag.load() || pauseFlag.load()) {
        try {
            if (requestBytes > 0) {
                if (existing > 0 && response.status_code == 206) {
                    // The server honored Range, so this chunk is safe to append.
                    std::ifstream in(tempPath, std::ios::binary);
                    std::ofstream out(filepath, std::ios::binary | std::ios::app);
                    out << in.rdbuf();
                    in.close();
                    out.close();
                    cpr::fs::remove(tempPath);
                    downloadedBytes = existing + requestBytes;
                } else if (existing == 0) {
                    // A partial response starting at byte 0 can be kept for the next resume.
                    if (!moveReplace(tempPath, filepath)) throw std::runtime_error("cannot preserve partial file");
                    downloadedBytes = requestBytes;
                } else {
                    // A CDN may ignore Range and return HTTP 200. Never append that response to
                    // an existing partial file, or the media would contain duplicated bytes.
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
        // Network loss/suspend can abort a transfer after useful bytes were received. Preserve a
        // safe partial chunk so a later source refresh can resume instead of starting from zero.
        try {
            if (requestBytes > 0) {
                if (existing > 0 && response.status_code == 206) {
                    std::ifstream in(tempPath, std::ios::binary);
                    std::ofstream out(filepath, std::ios::binary | std::ios::app);
                    out << in.rdbuf(); in.close(); out.close(); cpr::fs::remove(tempPath);
                    downloadedBytes = existing + requestBytes;
                } else if (existing == 0) {
                    if (moveReplace(tempPath, filepath)) downloadedBytes = requestBytes;
                } else if (cpr::fs::exists(tempPath)) {
                    cpr::fs::remove(tempPath);
                }
            } else if (cpr::fs::exists(tempPath)) cpr::fs::remove(tempPath);
        } catch (...) {}
        updateLiveProgress(taskId, downloadedBytes, totalBytes, audioPart);
        brls::Logger::error("DownloadManager: {} failed, HTTP {}, {}", url, response.status_code, response.error.message);
        return false;
    }

    try {
        if (existing > 0 && response.status_code == 206) {
            std::ifstream in(tempPath, std::ios::binary);
            std::ofstream out(filepath, std::ios::binary | std::ios::app);
            out << in.rdbuf(); in.close(); out.close(); cpr::fs::remove(tempPath);
            downloadedBytes = existing + requestBytes;
        } else {
            // If a resume request receives HTTP 200, Range was ignored. Replace the old
            // partial file with this complete response rather than appending it.
            if (!moveReplace(tempPath, filepath)) throw std::runtime_error("cannot finalize downloaded file");
            downloadedBytes = requestBytes;
            totalBytes = requestBytes;
        }
    } catch (const std::exception& e) {
        brls::Logger::error("DownloadManager: finalizing file failed: {}", e.what()); return false;
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
        if (!state->cv.wait_for(lock, std::chrono::seconds(20), [&]() { return state->done; })) {
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


bool DownloadManager::tryMuxDash(DownloadTask& task) {
    if (!task.is_dash || task.audio_file.empty()) return false;
    updateStage(task.id, DownloadTaskStage::MUXING);
    task.stage = DownloadTaskStage::MUXING;
    const auto videoPath = joinPath(task.dir, task.video_file);
    const auto audioPath = joinPath(task.dir, task.audio_file);
    if (!cpr::fs::exists(videoPath) || !cpr::fs::exists(audioPath) || !hasFfmpeg()) return false;

    const auto outputPath = joinPath(task.dir, "video.mp4");
    const std::string command = "ffmpeg -y -hide_banner -loglevel error -i " + shellQuote(videoPath) +
                                " -i " + shellQuote(audioPath) + " -map 0:v:0 -map 1:a:0 -c copy " +
                                shellQuote(outputPath);
    const int rc = std::system(command.c_str());
    if (rc == 0 && cpr::fs::exists(outputPath)) {
        try {
            if (cpr::fs::file_size(outputPath) > 0) {
                task.output_file = "video.mp4";
                task.muxed = true;
                // Source provenance is preserved in source.json/debug_source.json; keeping both
                // DASH tracks after a successful lossless mux would almost double storage use.
                try { cpr::fs::remove(videoPath); } catch (...) {}
                try { cpr::fs::remove(audioPath); } catch (...) {}
                return true;
            }
        } catch (...) {}
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
        f << "\nStable source metadata is in source.json. Expiring CDN URLs are kept separately in debug_source.json.\n";
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

    std::ofstream out(joinPath(task.dir, task.video_file), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& path : segFiles) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        out << in.rdbuf();
    }
    out.close();
    for (const auto& path : segFiles) { try { cpr::fs::remove(path); } catch (...) {} }
    task.total_bytes = task.downloaded_bytes;
    updateLiveProgress(task.id, task.downloaded_bytes, task.total_bytes, false);
    return true;
}

void DownloadManager::saveMetadata(const DownloadTask& task, bool completed) {
    try {
        cpr::fs::create_directories(task.dir);
        auto& appVersion = APPVersion::instance();
        nlohmann::json source = {
            {"schema_version", 2}, {"app", "wiliwili"}, {"app_version", appVersion.getVersionStr()},
            {"app_git_commit", appVersion.git_commit}, {"source_page_url", task.source_page_url}, {"title", task.title},
            {"series_title", task.series_title}, {"part_title", task.part_title}, {"part_index", task.part_index},
            {"owner_name", task.owner_name}, {"cover_url", task.cover_url}, {"bvid", task.bvid}, {"aid", task.aid},
            {"cid", task.cid}, {"is_pgc", task.is_pgc}, {"format", task.is_dash ? "dash" : "flv"},
            {"duration_seconds", task.duration_seconds}, {"created_at", task.created_at}, {"finished_at", task.finished_at},
            {"video", {{"quality_id", task.quality}, {"quality_desc", task.quality_desc}, {"codec_id", task.video_codec_id},
                       {"bandwidth", task.video_bandwidth}, {"width", task.video_width}, {"height", task.video_height},
                       {"file", task.video_file}}},
            {"audio", {{"id", task.audio_id}, {"desc", task.audio_desc}, {"codec_id", task.audio_codec_id},
                       {"bandwidth", task.audio_bandwidth}, {"file", task.audio_file}}},
            {"output_file", task.output_file}, {"muxed", task.muxed}
        };
        std::ofstream sf(joinPath(task.dir, "source.json"), std::ios::trunc);
        if (sf.is_open()) sf << source.dump(2);

        // CDN signatures expire quickly. Keep them for diagnostics/source provenance without
        // pretending they are a durable playback URL.
        nlohmann::json debugSource = {
            {"video_urls", task.video_urls}, {"audio_urls", task.audio_urls}, {"flv_segments", task.flv_segments}
        };
        std::ofstream df(joinPath(task.dir, "debug_source.json"), std::ios::trunc);
        if (df.is_open()) df << debugSource.dump(2);

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
            {"schema_version", 2}, {"id", task.id}, {"title", task.title}, {"series_title", task.series_title},
            {"part_title", task.part_title}, {"part_index", task.part_index}, {"owner_name", task.owner_name},
            {"cover_url", task.cover_url}, {"bvid", task.bvid}, {"aid", task.aid}, {"cid", task.cid},
            {"quality", task.quality}, {"quality_desc", task.quality_desc}, {"audio_id", task.audio_id},
            {"audio_desc", task.audio_desc}, {"is_dash", task.is_dash}, {"is_pgc", task.is_pgc},
            {"duration_seconds", task.duration_seconds}, {"estimated_bytes", estimateBytes(task)},
            {"video_file", task.video_file}, {"audio_file", task.audio_file}, {"output_file", task.output_file},
            {"downloaded_bytes", task.downloaded_bytes}, {"total_bytes", task.total_bytes},
            {"audio_downloaded_bytes", task.audio_downloaded_bytes}, {"audio_total_bytes", task.audio_total_bytes},
            {"download_dir", task.dir}, {"muxed", task.muxed}, {"created_at", task.created_at},
            {"finished_at", task.finished_at}, {"completed", completed}, {"source_file", "source.json"},
            {"debug_source_file", "debug_source.json"}, {"cover_file", "cover.jpg"}, {"danmaku_file", "danmaku.xml"},
            {"subtitle_files", subtitleFiles}
        };
        std::ofstream f(joinPath(task.dir, "info.json"), std::ios::trunc);
        if (f.is_open()) f << info.dump(2);
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
    BILI::get_danmaku(task.cid, [dir, state](const std::string& xml) {
        try {
            std::ofstream f(joinPath(dir, "danmaku.xml"), std::ios::trunc);
            if (f.is_open()) f << xml;
        } catch (...) {}
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
    state->cv.wait_for(lock, std::chrono::seconds(12), [&]() { return state->done; });
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
        if (!pageState->cv.wait_for(lock, std::chrono::seconds(12), [&]() { return pageState->done; }) || !pageState->ok)
            return;
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
            [dir, subtitle, finishOne](const bilibili::SubtitleData& data) {
                if (cpr::fs::exists(dir)) {
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
    subtitleState->cv.wait_for(lock, std::chrono::seconds(25), [&]() { return subtitleState->remaining == 0; });
}


void DownloadManager::runTask(const std::string& id) {
    DownloadTask work;
    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](const DownloadTask& t){ return t.id == id; });
        if (it == tasks.end()) {
            runningTasks.erase(id);
            return;
        }
        work = *it;
    }

    cpr::fs::create_directories(work.dir);
    bool success = false;
    bool fatalSpaceError = false;
    std::string lastError;

    // Suspend/resume and Wi-Fi roaming can invalidate an in-flight CDN connection. Retry with a
    // freshly resolved play-url a few times while keeping safe partial files for Range resume.
    for (int attempt = 0; attempt < 6 && !success; ++attempt) {
        if (work.cancelFlag->load() || work.pauseFlag->load()) break;

        updateStage(work.id, DownloadTaskStage::REFRESHING_SOURCE);
        work.stage = DownloadTaskStage::REFRESHING_SOURCE;
        const bool refreshed = refreshSource(work);
        const bool hasStoredSource = work.is_dash ? !work.video_urls.empty() : (!work.flv_segments.empty() || !work.video_urls.empty());
        if (!refreshed && !hasStoredSource) {
            lastError = "Unable to refresh the Bilibili media source";
            if (attempt < 5) {
                static const int delays[] = {2, 5, 10, 15, 20};
                std::this_thread::sleep_for(std::chrono::seconds(delays[attempt]));
                continue;
            }
            break;
        }

        work.estimated_bytes = estimateBytes(work);
        saveMetadata(work, false);

        updateStage(work.id, DownloadTaskStage::CHECKING_SPACE);
        work.stage = DownloadTaskStage::CHECKING_SPACE;
        const int64_t freeBytes = availableBytes(work.dir);
        const int64_t expected = estimateBytes(work);
        const int64_t already = work.downloaded_bytes + work.audio_downloaded_bytes;
        const int64_t remaining = expected > already ? expected - already : 0;
        constexpr int64_t SAFETY_MARGIN = 128LL * 1024LL * 1024LL;
        // ffmpeg stream-copy needs a temporary output file while the downloaded DASH tracks still
        // exist. Reserve approximately one final-file size so low-space devices do not fail only
        // after the complete download has finished.
        const int64_t muxReserve = work.is_dash && !work.audio_file.empty() && hasFfmpeg() ? expected : 0;
        const int64_t requiredFree = remaining + muxReserve + SAFETY_MARGIN;
        if (freeBytes >= 0 && requiredFree > 0 && freeBytes < requiredFree) {
            lastError = fmt::format("Not enough free space: need about {:.1f} MB, only {:.1f} MB available",
                                    requiredFree / 1048576.0, freeBytes / 1048576.0);
            fatalSpaceError = true;
            break;
        }

        if (ProgramConfig::instance().getBoolOption(SettingItem::DOWNLOAD_COVER) && !work.cover_url.empty()) {
            const auto coverPath = joinPath(work.dir, "cover.jpg");
            if (!cpr::fs::exists(coverPath)) {
                try {
                    auto session = bilibili::HTTP::createSession();
                    session->SetUrl(cpr::Url{work.cover_url});
                    session->SetTimeout(cpr::Timeout{10000});
                    auto response = session->Get();
                    if (!response.error && response.status_code == 200) {
                        std::ofstream f(coverPath, std::ios::binary);
                        if (f.is_open()) f.write(response.text.data(), static_cast<std::streamsize>(response.text.size()));
                    }
                } catch (...) {}
            }
        }

        success = work.is_dash ? downloadDash(work) : downloadFlv(work);
        if (!success && !work.cancelFlag->load() && !work.pauseFlag->load()) {
            lastError = "Download interrupted; the partial file was kept for resume";
            if (attempt < 5) {
                static const int delays[] = {2, 5, 10, 15, 20};
                std::this_thread::sleep_for(std::chrono::seconds(delays[attempt]));
            }
        }
    }

    if (success && work.is_dash) {
        if (!work.audio_file.empty()) {
            if (!tryMuxDash(work)) {
                // Raw DASH tracks are still a valid completed download: the built-in offline
                // player can open video+audio together even when ffmpeg is unavailable.
                work.output_file = work.video_file;
                if (hasFfmpeg()) lastError = "Downloaded successfully, but ffmpeg muxing failed; raw DASH tracks were kept";
            }
        } else {
            work.output_file = work.video_file;
        }
    }
    if (success && !work.is_dash) work.output_file = work.video_file;

    bool completed = false;
    bool staleWorker = false;
    std::string title;

    if (success) {
        // Keep the task in DOWNLOADING/FETCHING_EXTRAS until best-effort offline assets have
        // finished (or timed out), so the visible stage accurately represents what is happening.
        work.stage = DownloadTaskStage::FETCHING_EXTRAS;
        updateStage(work.id, DownloadTaskStage::FETCHING_EXTRAS);
        saveMetadata(work, false);
        writeReadme(work);
        if (ProgramConfig::instance().getBoolOption(SettingItem::DOWNLOAD_DANMAKU)) saveDanmaku(work);
        if (ProgramConfig::instance().getBoolOption(SettingItem::DOWNLOAD_SUBTITLES)) saveSubtitles(work);
        work.finished_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    {
        std::lock_guard<std::mutex> lock(tasksMutex);
        for (auto& t : tasks) if (t.id == id) {
            // A quick pause->resume replaces the flags. Do not let an old worker clobber the new generation.
            staleWorker = t.cancelFlag != work.cancelFlag || t.pauseFlag != work.pauseFlag;
            if (staleWorker) break;
            t.downloaded_bytes = work.downloaded_bytes; t.total_bytes = work.total_bytes;
            t.audio_downloaded_bytes = work.audio_downloaded_bytes; t.audio_total_bytes = work.audio_total_bytes;
            t.video_urls = work.video_urls; t.audio_urls = work.audio_urls; t.flv_segments = work.flv_segments;
            t.video_codec_id = work.video_codec_id; t.video_bandwidth = work.video_bandwidth;
            t.video_width = work.video_width; t.video_height = work.video_height;
            t.audio_codec_id = work.audio_codec_id; t.audio_bandwidth = work.audio_bandwidth;
            t.estimated_bytes = work.estimated_bytes; t.output_file = work.output_file; t.muxed = work.muxed;
            t.finished_at = work.finished_at; t.stage = work.stage;
            if (work.cancelFlag->load()) t.status = DownloadTaskStatus::CANCELLED;
            else if (work.pauseFlag->load()) t.status = DownloadTaskStatus::PAUSED;
            else if (success) {
                t.status = DownloadTaskStatus::COMPLETED;
                t.stage = DownloadTaskStage::COMPLETED;
                // A mux warning is useful, but does not turn a playable offline task into FAILED.
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

    if (completed) {
        saveMetadata(work, true);
        writeReadme(work);
    } else if (!staleWorker) {
        saveMetadata(work, false);
    }

    saveState();
    brls::sync([this, id, completed, title, staleWorker]() {
        if (!staleWorker) taskStatusChangedEvent.fire(id);
        if (completed) brls::Application::notify("wiliwili/player/download/completed"_i18n + (title.empty() ? "" : ": " + title));
    });
    startNextTask();
}
