#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <borealis/core/event.hpp>
#include <borealis/core/singleton.hpp>
#include <nlohmann/json.hpp>

#include "utils/event_helper.hpp"

struct FlvSegment {
    std::string url;
    std::vector<std::string> backup_urls;
    uint64_t size = 0;
    int order = 0;
};

inline void to_json(nlohmann::json& j, const FlvSegment& s) {
    j = nlohmann::json{{"url", s.url}, {"backup_urls", s.backup_urls}, {"size", s.size}, {"order", s.order}};
}
inline void from_json(const nlohmann::json& j, FlvSegment& s) {
    s.url = j.value("url", "");
    s.backup_urls = j.value("backup_urls", std::vector<std::string>{});
    s.size = j.value("size", uint64_t{0});
    s.order = j.value("order", 0);
}

enum class DownloadTaskStatus { PENDING, DOWNLOADING, PAUSED, COMPLETED, FAILED, CANCELLED };
enum class DownloadTaskStage {
    QUEUED,
    REFRESHING_SOURCE,
    CHECKING_SPACE,
    DOWNLOADING_VIDEO,
    DOWNLOADING_AUDIO,
    MUXING,
    FETCHING_EXTRAS,
    COMPLETED
};

struct DownloadTask {
    int schema_version = 2;
    std::string id;
    std::string bvid;
    uint64_t cid = 0;
    uint64_t aid = 0;
    std::string title;
    std::string series_title;
    std::string part_title;
    int part_index = 0;
    int duration_seconds = 0;
    std::string owner_name;
    std::string cover_url;
    std::string source_page_url;

    int quality = 0;
    std::string quality_desc;
    int video_codec_id = 0;
    unsigned int video_bandwidth = 0;
    int video_width = 0;
    int video_height = 0;

    int audio_id = 0;
    std::string audio_desc;
    int audio_codec_id = 0;
    unsigned int audio_bandwidth = 0;

    bool is_dash = false;
    bool is_pgc = false;
    std::vector<std::string> video_urls;
    std::vector<std::string> audio_urls;
    std::vector<FlvSegment> flv_segments;

    DownloadTaskStatus status = DownloadTaskStatus::PENDING;
    DownloadTaskStage stage = DownloadTaskStage::QUEUED;
    std::string error_message;
    int64_t estimated_bytes = 0;
    int64_t downloaded_bytes = 0;
    int64_t total_bytes = 0;
    int64_t audio_downloaded_bytes = 0;
    int64_t audio_total_bytes = 0;

    std::string dir;
    std::string video_file;
    std::string audio_file;
    std::string output_file;
    bool muxed = false;
    int64_t created_at = 0;
    int64_t finished_at = 0;

    std::shared_ptr<std::atomic<bool>> cancelFlag;
    std::shared_ptr<std::atomic<bool>> pauseFlag;
};

inline void to_json(nlohmann::json& j, const DownloadTask& t) {
    j = nlohmann::json{
        {"schema_version", t.schema_version}, {"id", t.id}, {"bvid", t.bvid}, {"cid", t.cid}, {"aid", t.aid},
        {"title", t.title}, {"series_title", t.series_title}, {"part_title", t.part_title}, {"part_index", t.part_index},
        {"duration_seconds", t.duration_seconds}, {"owner_name", t.owner_name}, {"cover_url", t.cover_url},
        {"source_page_url", t.source_page_url}, {"quality", t.quality}, {"quality_desc", t.quality_desc},
        {"video_codec_id", t.video_codec_id}, {"video_bandwidth", t.video_bandwidth}, {"video_width", t.video_width},
        {"video_height", t.video_height}, {"audio_id", t.audio_id}, {"audio_desc", t.audio_desc},
        {"audio_codec_id", t.audio_codec_id}, {"audio_bandwidth", t.audio_bandwidth}, {"is_dash", t.is_dash},
        {"is_pgc", t.is_pgc}, {"video_urls", t.video_urls}, {"audio_urls", t.audio_urls},
        {"flv_segments", t.flv_segments}, {"status", static_cast<int>(t.status)}, {"stage", static_cast<int>(t.stage)},
        {"error_message", t.error_message}, {"estimated_bytes", t.estimated_bytes}, {"downloaded_bytes", t.downloaded_bytes},
        {"total_bytes", t.total_bytes}, {"audio_downloaded_bytes", t.audio_downloaded_bytes},
        {"audio_total_bytes", t.audio_total_bytes}, {"dir", t.dir}, {"video_file", t.video_file},
        {"audio_file", t.audio_file}, {"output_file", t.output_file}, {"muxed", t.muxed},
        {"created_at", t.created_at}, {"finished_at", t.finished_at}};
}

inline void from_json(const nlohmann::json& j, DownloadTask& t) {
    t.schema_version = j.value("schema_version", 1);
    t.id = j.value("id", ""); t.bvid = j.value("bvid", ""); t.cid = j.value("cid", uint64_t{0});
    t.aid = j.value("aid", uint64_t{0}); t.title = j.value("title", "");
    t.series_title = j.value("series_title", t.title); t.part_title = j.value("part_title", "");
    t.part_index = j.value("part_index", 0); t.duration_seconds = j.value("duration_seconds", 0);
    t.owner_name = j.value("owner_name", ""); t.cover_url = j.value("cover_url", "");
    t.source_page_url = j.value("source_page_url", ""); t.quality = j.value("quality", 0);
    t.quality_desc = j.value("quality_desc", ""); t.video_codec_id = j.value("video_codec_id", 0);
    t.video_bandwidth = j.value("video_bandwidth", 0u); t.video_width = j.value("video_width", 0);
    t.video_height = j.value("video_height", 0); t.audio_id = j.value("audio_id", 0);
    t.audio_desc = j.value("audio_desc", ""); t.audio_codec_id = j.value("audio_codec_id", 0);
    t.audio_bandwidth = j.value("audio_bandwidth", 0u); t.is_dash = j.value("is_dash", false);
    t.is_pgc = j.value("is_pgc", false); t.video_urls = j.value("video_urls", std::vector<std::string>{});
    t.audio_urls = j.value("audio_urls", std::vector<std::string>{});
    t.flv_segments = j.value("flv_segments", std::vector<FlvSegment>{});
    t.status = static_cast<DownloadTaskStatus>(j.value("status", 0));
    t.stage = static_cast<DownloadTaskStage>(j.value("stage", static_cast<int>(DownloadTaskStage::QUEUED)));
    t.error_message = j.value("error_message", ""); t.estimated_bytes = j.value("estimated_bytes", int64_t{0});
    t.downloaded_bytes = j.value("downloaded_bytes", int64_t{0}); t.total_bytes = j.value("total_bytes", int64_t{0});
    t.audio_downloaded_bytes = j.value("audio_downloaded_bytes", int64_t{0});
    t.audio_total_bytes = j.value("audio_total_bytes", int64_t{0}); t.dir = j.value("dir", "");
    t.video_file = j.value("video_file", ""); t.audio_file = j.value("audio_file", "");
    t.output_file = j.value("output_file", ""); t.muxed = j.value("muxed", false);
    t.created_at = j.value("created_at", int64_t{0}); t.finished_at = j.value("finished_at", int64_t{0});
    t.cancelFlag = std::make_shared<std::atomic<bool>>(false);
    t.pauseFlag = std::make_shared<std::atomic<bool>>(false);
}

class DownloadManager : public brls::Singleton<DownloadManager> {
public:
    DownloadManager();
    ~DownloadManager();

    void initRuntimeHooks();
    void loadState();
    void saveState();
    void addTask(DownloadTask task);
    void addTasks(std::vector<DownloadTask> batch);
    void pauseTask(const std::string& id);
    void resumeTask(const std::string& id);
    void retryTask(const std::string& id);
    void cancelTask(const std::string& id);
    void deleteTask(const std::string& id);
    void moveTaskUp(const std::string& id);
    void moveTaskDown(const std::string& id);

    std::vector<DownloadTask> getTasksSnapshot() const;
    bool getTaskSnapshot(const std::string& id, DownloadTask& out) const;
    bool hasIncompleteDownloads() const;
    bool hasCompletedTask(const std::string& bvid, uint64_t cid) const;

    brls::Event<std::string>* getTaskProgressEvent() { return &taskProgressEvent; }
    brls::Event<std::string>* getTaskStatusChangedEvent() { return &taskStatusChangedEvent; }

    static std::string defaultDownloadDir();
    static int64_t estimateBytes(const DownloadTask& task);
    static int64_t availableBytes(const std::string& path);
    static std::string playableVideoPath(const DownloadTask& task);
    static std::string playableAudioPath(const DownloadTask& task);

private:
    mutable std::mutex tasksMutex;
    std::mutex stateFileMutex;
    std::vector<DownloadTask> tasks;
    std::unordered_set<std::string> runningTasks;
    brls::Event<std::string> taskProgressEvent;
    brls::Event<std::string> taskStatusChangedEvent;
    MPVEvent::Subscription mpvEventSubscription{};
    bool runtimeHooksInitialized = false;
    std::atomic<bool> playbackActive{false};

    int maxConcurrent() const;
    int64_t effectiveSpeedLimit() const;
    void startNextTask();
    void runTask(const std::string& id);
    bool refreshSource(DownloadTask& task);
    bool downloadDash(DownloadTask& task);
    bool downloadFlv(DownloadTask& task);
    bool tryMuxDash(DownloadTask& task);
    void writeReadme(const DownloadTask& task);
    bool downloadFile(const std::string& url, const std::string& filepath,
                      std::atomic<bool>& cancelFlag, std::atomic<bool>& pauseFlag,
                      int64_t& downloadedBytes, int64_t& totalBytes,
                      const std::string& taskId, bool audioPart = false);
    void updateLiveProgress(const std::string& taskId, int64_t downloaded, int64_t total, bool audioPart);
    void updateStage(const std::string& taskId, DownloadTaskStage stage, const std::string& error = "");
    void saveMetadata(const DownloadTask& task, bool completed);
    void saveDanmaku(const DownloadTask& task);
    void saveSubtitles(const DownloadTask& task);
};
