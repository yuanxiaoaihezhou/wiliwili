#include "activity/download_activity.hpp"

#include <algorithm>
#include <cctype>
#include <cpr/filesystem.h>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fmt/format.h>

#include <borealis/core/application.hpp>
#include <borealis/views/dialog.hpp>

#include "utils/config_helper.hpp"
#include "utils/dialog_helper.hpp"
#include "view/mpv_core.hpp"
#include "view/video_view.hpp"

using namespace brls::literals;


static std::string shellQuotePath(const std::string& value) {
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

static bool openLocalDirectory(const std::string& path) {
    if (path.empty() || !cpr::fs::exists(path)) return false;
#ifdef _WIN32
    const std::string command = "explorer " + shellQuotePath(path);
#elif defined(__APPLE__)
    const std::string command = "open " + shellQuotePath(path) + " >/dev/null 2>&1 &";
#elif defined(__linux__)
    const std::string command = "xdg-open " + shellQuotePath(path) + " >/dev/null 2>&1 &";
#else
    return false;
#endif
    return std::system(command.c_str()) == 0;
}

static std::string finishedDate(int64_t seconds) {
    if (seconds <= 0) return {};
    const std::time_t when = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &when) != 0) return {};
#else
    if (localtime_r(&when, &local) == nullptr) return {};
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local) == 0) return {};
    return buffer;
}

static std::string humanBytes(int64_t value) {
    if (value <= 0) return "0 B";
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double n = static_cast<double>(value); int unit = 0;
    while (n >= 1024.0 && unit < 4) { n /= 1024.0; ++unit; }
    std::ostringstream ss; ss << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << n << " " << units[unit];
    return ss.str();
}

static std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static std::string selectOfflineSubtitle(const DownloadTask& task) {
    std::vector<std::string> candidates;
    try {
        for (const auto& entry : cpr::fs::directory_iterator(task.dir)) {
            if (!cpr::fs::is_regular_file(entry.path())) continue;
            const auto ext = lowerAscii(entry.path().extension().string());
            if (ext == ".ass" || ext == ".srt") candidates.emplace_back(entry.path().string());
        }
    } catch (...) {}
    if (candidates.empty()) return {};

    const auto locale = lowerAscii(brls::Application::getLocale());
    std::vector<std::string> languageHints;
    if (locale.find("zh-hans") != std::string::npos || locale.find("zh-cn") != std::string::npos)
        languageHints = {"zh-cn", "zh-hans", "zh"};
    else if (locale.find("zh-hant") != std::string::npos || locale.find("zh-tw") != std::string::npos)
        languageHints = {"zh-tw", "zh-hant", "zh"};
    else if (locale.find("ja") != std::string::npos) languageHints = {"ja", "jp"};
    else if (locale.find("ko") != std::string::npos) languageHints = {"ko"};
    else if (locale.find("it") != std::string::npos) languageHints = {"it"};
    else languageHints = {"en"};

    std::stable_sort(candidates.begin(), candidates.end(), [&](const std::string& a, const std::string& b) {
        auto score = [&](const std::string& path) {
            const auto name = lowerAscii(cpr::fs::path(path).filename().string());
            int value = lowerAscii(cpr::fs::path(path).extension().string()) == ".ass" ? 2 : 0;
            for (size_t i = 0; i < languageHints.size(); ++i) {
                if (name.find(languageHints[i]) != std::string::npos) value += 100 - static_cast<int>(i) * 10;
            }
            return value;
        };
        return score(a) > score(b);
    });
    return candidates.front();
}

static std::string mpvFixedLengthPath(const std::string& path) {
    // mpv's fixed-length syntax safely embeds commas/equals/quotes inside suboption values.
    return "%" + std::to_string(path.size()) + "%" + path;
}

static std::string statusText(DownloadTaskStatus status) {
    switch (status) {
        case DownloadTaskStatus::PENDING: return "wiliwili/download_manager/status/queued"_i18n;
        case DownloadTaskStatus::DOWNLOADING: return "wiliwili/download_manager/status/downloading"_i18n;
        case DownloadTaskStatus::PAUSED: return "wiliwili/download_manager/status/paused"_i18n;
        case DownloadTaskStatus::COMPLETED: return "wiliwili/download_manager/status/completed"_i18n;
        case DownloadTaskStatus::FAILED: return "wiliwili/download_manager/status/failed"_i18n;
        case DownloadTaskStatus::CANCELLED: return "wiliwili/download_manager/status/cancelled"_i18n;
    }
    return "";
}

static std::string stageText(DownloadTaskStage stage) {
    switch (stage) {
        case DownloadTaskStage::QUEUED: return "wiliwili/download_manager/stage/queued"_i18n;
        case DownloadTaskStage::REFRESHING_SOURCE: return "wiliwili/download_manager/stage/source"_i18n;
        case DownloadTaskStage::CHECKING_SPACE: return "wiliwili/download_manager/stage/space"_i18n;
        case DownloadTaskStage::DOWNLOADING_VIDEO: return "wiliwili/download_manager/stage/video"_i18n;
        case DownloadTaskStage::DOWNLOADING_AUDIO: return "wiliwili/download_manager/stage/audio"_i18n;
        case DownloadTaskStage::MUXING: return "wiliwili/download_manager/stage/muxing"_i18n;
        case DownloadTaskStage::VERIFYING: return "wiliwili/download_manager/stage/verifying"_i18n;
        case DownloadTaskStage::FETCHING_EXTRAS: return "wiliwili/download_manager/stage/extras"_i18n;
        case DownloadTaskStage::COMPLETED: return "wiliwili/download_manager/stage/done"_i18n;
    }
    return "";
}

DownloadCard::DownloadCard() {
    inflateFromXMLRes("xml/views/download_card.xml");
    const auto profile = ProgramConfig::instance().getSettingItem(SettingItem::APP_UI_PROFILE, std::string{"auto"});
    if (profile == "handheld") setHeight(132);
}
RecyclingGridItem* DownloadCard::create() { return new DownloadCard(); }

void DownloadCard::setTask(const DownloadTask& task) {
    titleLabel->setText(task.title.empty() ? task.bvid : task.title);
    std::string meta = task.quality_desc.empty() ? std::to_string(task.quality) : task.quality_desc;
    if (task.video_codec_id != 0) {
        if (task.video_codec_id == 7) meta += " · AVC";
        else if (task.video_codec_id == 12) meta += " · HEVC";
        else if (task.video_codec_id == 13) meta += " · AV1";
    }
    if (!task.audio_desc.empty()) meta += " · " + task.audio_desc;
    if (!task.owner_name.empty()) meta += " · " + task.owner_name;
    if (!task.part_title.empty() && task.part_title != task.title && task.title.find(task.part_title) == std::string::npos)
        meta += " · " + task.part_title;
    if (task.status == DownloadTaskStatus::COMPLETED) {
        const auto date = finishedDate(task.finished_at);
        if (!date.empty()) meta += " · " + date;
    }
    metaLabel->setText(meta);

    const int64_t done = task.downloaded_bytes + task.audio_downloaded_bytes;
    const int64_t total = task.total_bytes + task.audio_total_bytes;
    std::string progress;
    if (task.status == DownloadTaskStatus::FAILED && !task.error_message.empty()) {
        progress = task.error_message;
    } else if (task.status == DownloadTaskStatus::COMPLETED) {
        progress = stageText(DownloadTaskStage::COMPLETED) + " · " + humanBytes(done);
        if (!task.error_message.empty()) progress += " · " + task.error_message;
    } else if (total > 0) {
        const int percent = static_cast<int>(std::min<int64_t>(100, done * 100 / std::max<int64_t>(1, total)));
        progress = fmt::format("{} · {} / {} · {}%", stageText(task.stage), humanBytes(done), humanBytes(total), percent);
    } else {
        const auto estimate = DownloadManager::estimateBytes(task);
        progress = stageText(task.stage) + " · " + humanBytes(done);
        if (estimate > 0) progress += " / ~" + humanBytes(estimate);
    }
    progressLabel->setText(progress);
    statusLabel->setText(statusText(task.status));
}

static void playOfflineTask(const DownloadTask& task) {
    const auto video = DownloadManager::playableVideoPath(task);
    if (video.empty()) {
        brls::Application::notify("wiliwili/download_manager/play_missing"_i18n);
        return;
    }
    brls::Application::pushActivity(new OfflinePlayerActivity(task));
}

class DownloadDataSource : public RecyclingGridDataSource {
public:
    DownloadDataSource(std::vector<DownloadTask> tasks, bool libraryMode)
        : tasks(std::move(tasks)), libraryMode(libraryMode) {}

    size_t getItemCount() override { return tasks.size(); }
    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        auto* cell = static_cast<DownloadCard*>(recycler->dequeueReusableCell("Cell"));
        cell->setTask(tasks[index]);
        return cell;
    }
    void onItemSelected(RecyclingGrid*, size_t index) override {
        if (index >= tasks.size()) return;
        const auto task = tasks[index];
        auto* dialog = new brls::Dialog(task.title.empty() ? task.bvid : task.title);
        if (task.status == DownloadTaskStatus::COMPLETED) {
            dialog->addButton("wiliwili/download_manager/play"_i18n, [task]() { playOfflineTask(task); });
        } else if (task.status == DownloadTaskStatus::DOWNLOADING) {
            dialog->addButton("wiliwili/download_manager/pause"_i18n, [id = task.id]() { DownloadManager::instance().pauseTask(id); });
            dialog->addButton("wiliwili/download_manager/cancel"_i18n, [id = task.id]() { DownloadManager::instance().cancelTask(id); });
        } else if (task.status == DownloadTaskStatus::PAUSED) {
            dialog->addButton("wiliwili/download_manager/resume"_i18n, [id = task.id]() { DownloadManager::instance().resumeTask(id); });
            dialog->addButton("wiliwili/download_manager/cancel"_i18n, [id = task.id]() { DownloadManager::instance().cancelTask(id); });
        } else if (task.status == DownloadTaskStatus::FAILED || task.status == DownloadTaskStatus::CANCELLED) {
            dialog->addButton("wiliwili/download_manager/retry"_i18n, [id = task.id]() { DownloadManager::instance().retryTask(id); });
        } else if (task.status == DownloadTaskStatus::PENDING) {
            dialog->addButton("wiliwili/download_manager/cancel"_i18n, [id = task.id]() { DownloadManager::instance().cancelTask(id); });
        }

        if (!libraryMode && (task.status == DownloadTaskStatus::PENDING || task.status == DownloadTaskStatus::PAUSED)) {
            dialog->addButton("wiliwili/download_manager/move_up"_i18n, [id = task.id]() { DownloadManager::instance().moveTaskUp(id); });
            dialog->addButton("wiliwili/download_manager/move_down"_i18n, [id = task.id]() { DownloadManager::instance().moveTaskDown(id); });
        }
        if (!task.dir.empty()) {
            dialog->addButton("wiliwili/download_manager/open_folder"_i18n, [path = task.dir]() {
                if (!openLocalDirectory(path)) brls::Application::notify(path);
            });
            dialog->addButton("wiliwili/download_manager/source_info"_i18n, [task]() {
                std::string message = (cpr::fs::path(task.dir) / "source.json").string();
                if (!task.source_page_url.empty()) message += "\n" + task.source_page_url;
                brls::Application::notify(message);
            });
        }
        if (!task.source_page_url.empty()) {
            dialog->addButton("wiliwili/download_manager/open_source"_i18n, [url = task.source_page_url]() {
                brls::Application::getPlatform()->openBrowser(url);
            });
        }
        if (!task.error_message.empty()) {
            dialog->addButton("wiliwili/download_manager/show_error"_i18n, [error = task.error_message]() { brls::Application::notify(error); });
        }
        if (task.status != DownloadTaskStatus::DOWNLOADING) {
            dialog->addButton("wiliwili/download_manager/delete"_i18n, [id = task.id, title = task.title]() {
                auto* confirm = new brls::Dialog("wiliwili/download_manager/delete_confirm"_i18n + (title.empty() ? "" : "\n" + title));
                confirm->addButton("hints/cancel"_i18n, []() {});
                confirm->addButton("wiliwili/download_manager/delete"_i18n, [id]() { DownloadManager::instance().deleteTask(id); });
                confirm->open();
            });
        }
        dialog->addButton("hints/cancel"_i18n, []() {});
        dialog->open();
    }
    void clearData() override { tasks.clear(); }

    bool updateTask(const DownloadTask& task, size_t& index) {
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i].id != task.id) continue;
            tasks[i] = task;
            index = i;
            return true;
        }
        return false;
    }

private:
    std::vector<DownloadTask> tasks;
    bool libraryMode = false;
};

void DownloadActivity::onContentAvailable() {
    grid->registerCell("Cell", []() { return DownloadCard::create(); });
    const auto profile = ProgramConfig::instance().getSettingItem(SettingItem::APP_UI_PROFILE, std::string{"auto"});
    grid->estimatedRowHeight = profile == "handheld" ? 132 : 118;
    reload();
    registerAction("wiliwili/download_manager/offline_library"_i18n, brls::ControllerButton::BUTTON_Y,
                   [](brls::View*) { brls::Application::pushActivity(new OfflineLibraryActivity()); return true; }, true);
    progressSub = DownloadManager::instance().getTaskProgressEvent()->subscribe([this](const std::string& id) {
        // Progress arrives several times per second. Replacing the entire data source here resets
        // scroll/focus state and feels especially bad with a controller. Update only the matching
        // visible card; status/queue changes still use reload() because they can reorder rows.
        DownloadTask task;
        if (DownloadManager::instance().getTaskSnapshot(id, task)) {
            auto* dataSource = dynamic_cast<DownloadDataSource*>(grid->getDataSource());
            size_t index = 0;
            if (dataSource && dataSource->updateTask(task, index)) {
                auto* card = dynamic_cast<DownloadCard*>(grid->getGridItemByIndex(index));
                if (card) card->setTask(task);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastProgressRefresh).count() >= 1.0) {
            lastProgressRefresh = now;
            const auto dir = DownloadManager::defaultDownloadDir();
            const auto free = DownloadManager::availableBytes(dir);
            summaryLabel->setText(dir + (free >= 0 ? " · " + "wiliwili/player/download/free_space"_i18n + ": " + humanBytes(free) : ""));
        }
    });
    statusSub = DownloadManager::instance().getTaskStatusChangedEvent()->subscribe([this](const std::string&) { reload(); });
}

void DownloadActivity::reload() {
    const auto dir = DownloadManager::defaultDownloadDir();
    const auto free = DownloadManager::availableBytes(dir);
    summaryLabel->setText(dir + (free >= 0 ? " · " + "wiliwili/player/download/free_space"_i18n + ": " + humanBytes(free) : ""));
    auto tasks = DownloadManager::instance().getTasksSnapshot();
    // Preserve queue order for active/pending tasks so move-up/down is meaningful; completed items
    // are grouped below the queue by completion time.
    std::stable_sort(tasks.begin(), tasks.end(), [](const DownloadTask& a, const DownloadTask& b) {
        const bool ac = a.status == DownloadTaskStatus::COMPLETED;
        const bool bc = b.status == DownloadTaskStatus::COMPLETED;
        if (ac != bc) return !ac;
        if (ac && bc) return a.finished_at > b.finished_at;
        return false;
    });
    grid->setDataSource(new DownloadDataSource(std::move(tasks), false));
    if (grid->getItemCount() == 0) grid->setEmpty("wiliwili/download_manager/empty"_i18n);
}

DownloadActivity::~DownloadActivity() {
    DownloadManager::instance().getTaskProgressEvent()->unsubscribe(progressSub);
    DownloadManager::instance().getTaskStatusChangedEvent()->unsubscribe(statusSub);
}

void OfflineLibraryActivity::onContentAvailable() {
    grid->registerCell("Cell", []() { return DownloadCard::create(); });
    grid->estimatedRowHeight = 118;
    reload();
    statusSub = DownloadManager::instance().getTaskStatusChangedEvent()->subscribe([this](const std::string&) { reload(); });
}

void OfflineLibraryActivity::reload() {
    auto all = DownloadManager::instance().getTasksSnapshot();
    std::vector<DownloadTask> completed;
    for (auto& task : all) if (task.status == DownloadTaskStatus::COMPLETED) completed.emplace_back(std::move(task));
    std::stable_sort(completed.begin(), completed.end(), [](const auto& a, const auto& b) { return a.finished_at > b.finished_at; });
    grid->setDataSource(new DownloadDataSource(std::move(completed), true));
    if (grid->getItemCount() == 0) grid->setEmpty("wiliwili/download_manager/library_empty"_i18n);
}

OfflineLibraryActivity::~OfflineLibraryActivity() {
    DownloadManager::instance().getTaskStatusChangedEvent()->unsubscribe(statusSub);
}

void OfflinePlayerActivity::onContentAvailable() {
    title->setText(task.title);
    video->registerCommonActions(this);
    video->setTitle(task.title);
    video->setQuality(task.quality_desc);
    video->hideDLNAButton();
    video->hideVideoQualityButton();
    video->hideHistorySetting();
    video->hideVideoRelatedSetting();
    video->hideHighlightLineSetting();
    video->hideSkipOpeningCreditsSetting();
    const auto profile = ProgramConfig::instance().getSettingItem(SettingItem::APP_UI_PROFILE, std::string{"auto"});
    if (profile == "handheld" || profile == "tv") video->setTvControlMode(true);

    const auto videoPath = DownloadManager::playableVideoPath(task);
    const auto audioPath = DownloadManager::playableAudioPath(task);
    if (videoPath.empty()) {
        brls::Application::notify("wiliwili/download_manager/play_missing"_i18n);
        return;
    }

    const auto subtitlePath = selectOfflineSubtitle(task);

    // loadfile's fourth argument is a comma-separated suboption list. mpv documents its
    // fixed-length %n%... syntax specifically for paths containing suboption separators.
    std::string extra;
    if (!audioPath.empty()) extra += "audio-file=" + mpvFixedLengthPath(audioPath);
    if (!subtitlePath.empty()) {
        if (!extra.empty()) extra += ",";
        extra += "sub-file=" + mpvFixedLengthPath(subtitlePath);
    }
    MPVCore::instance().setUrl(videoPath, extra);
}
