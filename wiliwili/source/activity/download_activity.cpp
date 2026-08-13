#include "activity/download_activity.hpp"

#include <algorithm>
#include <cctype>
#include <cpr/filesystem.h>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <fmt/format.h>

#include <borealis/core/application.hpp>
#include <borealis/views/dialog.hpp>

#include "utils/config_helper.hpp"
#include "utils/image_helper.hpp"
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
}

DownloadCard::~DownloadCard() { ImageHelper::clear(coverImage); }

RecyclingGridItem* DownloadCard::create() { return new DownloadCard(); }

void DownloadCard::prepareForReuse() {
    // RecyclingGrid can reuse a card for another collection item with the same cover URL.
    // Reset both the request and URL key; otherwise setTask() sees the same URL and leaves
    // the placeholder image in place.
    ImageHelper::clear(coverImage);
    loadedCoverUrl.clear();
    coverImage->setImageFromRes("pictures/video-card-bg.png");
}

void DownloadCard::cacheForReuse() {
    ImageHelper::clear(coverImage);
    loadedCoverUrl.clear();
}

void DownloadCard::setTask(const DownloadTask& task) {
    const std::string displayTitle = task.title.empty() ? task.bvid : task.title;
    titleLabel->setIsWrapping(true);
    titleLabel->setText(displayTitle);

    std::string cover = task.cover_url;
    if (!cover.empty()) cover += ImageHelper::h_ext;
    if (cover != loadedCoverUrl) {
        ImageHelper::clear(coverImage);
        coverImage->setImageFromRes("pictures/video-card-bg.png");
        loadedCoverUrl = cover;
        if (!cover.empty()) ImageHelper::with(coverImage)->load(cover);
    }

    // Recommendation-style information hierarchy: author first, then the media choices that
    // distinguish this offline copy. Keep the progress/status information on the image itself.
    std::string meta = task.owner_name;
    auto appendMeta = [&](const std::string& value) {
        if (value.empty()) return;
        if (!meta.empty()) meta += " · ";
        meta += value;
    };
    appendMeta(task.quality_desc.empty() ? (task.quality > 0 ? std::to_string(task.quality) : std::string{}) : task.quality_desc);
    if (task.video_codec_id == 7) appendMeta("AVC");
    else if (task.video_codec_id == 12) appendMeta("HEVC");
    else if (task.video_codec_id == 13) appendMeta("AV1");
    appendMeta(task.audio_desc);
    if (meta.empty() && !task.part_title.empty()) meta = task.part_title;
    metaLabel->setText(meta);

    const int64_t done = std::max<int64_t>(0, task.downloaded_bytes) + std::max<int64_t>(0, task.audio_downloaded_bytes);
    const int64_t knownTotal = std::max<int64_t>(0, task.total_bytes) + std::max<int64_t>(0, task.audio_total_bytes);
    const int64_t estimate = DownloadManager::estimateBytes(task);
    const int64_t total = knownTotal > 0 ? knownTotal : std::max<int64_t>(0, estimate);
    float ratio = 0.0f;
    if (task.status == DownloadTaskStatus::COMPLETED) ratio = 1.0f;
    else if (total > 0) ratio = static_cast<float>(std::min<double>(1.0, static_cast<double>(done) / total));
    progressBar->setWidthPercentage(ratio * 100.0f);

    std::string progressText;
    std::string percentText;
    if (task.status == DownloadTaskStatus::COMPLETED) {
        progressText = humanBytes(done > 0 ? done : total);
        const auto date = finishedDate(task.finished_at);
        percentText = date.empty() ? "100%" : date;
    } else if (task.status == DownloadTaskStatus::FAILED) {
        progressText = stageText(task.stage);
        if (done > 0) progressText += " · " + humanBytes(done);
        percentText = "!";
    } else {
        progressText = stageText(task.stage);
        if (total > 0) progressText += " · " + humanBytes(done) + " / " + humanBytes(total);
        else if (done > 0) progressText += " · " + humanBytes(done);
        percentText = total > 0 ? fmt::format("{}%", static_cast<int>(ratio * 100.0f)) : "--";
    }
    progressLabel->setText(progressText);
    percentLabel->setText(percentText);
    statusLabel->setText(statusText(task.status));

    auto theme = brls::Application::getTheme();
    if (task.status == DownloadTaskStatus::FAILED || task.status == DownloadTaskStatus::CANCELLED) {
        statusBox->setBackgroundColor(theme.getColor("color/tip/red"));
    } else if (task.status == DownloadTaskStatus::COMPLETED || task.status == DownloadTaskStatus::PAUSED ||
               task.status == DownloadTaskStatus::PENDING) {
        statusBox->setBackgroundColor(theme.getColor("color/grey_4"));
    } else {
        statusBox->setBackgroundColor(theme.getColor("color/bilibili"));
    }
}

static void playOfflineTask(const DownloadTask& task) {
    const auto video = DownloadManager::playableVideoPath(task);
    if (video.empty()) {
        brls::Application::notify("wiliwili/download_manager/play_missing"_i18n);
        return;
    }
    brls::Application::pushActivity(new OfflinePlayerActivity(task));
}


static std::string downloadTaskGroupKey(const DownloadTask& task) {
    if (task.dir.empty()) return {};
    try {
        const auto parent = cpr::fs::path(task.dir).parent_path().string();
        if (!parent.empty()) return parent;
    } catch (...) {}
    return {};
}

static DownloadTask makeCollectionDisplayTask(const std::vector<DownloadTask>& children) {
    DownloadTask display;
    if (children.empty()) return display;

    display = children.front();
    display.id = "collection:" + downloadTaskGroupKey(children.front());
    for (const auto& child : children) {
        if (!child.series_cover_url.empty()) {
            display.cover_url = child.series_cover_url;
            break;
        }
    }
    display.title = children.front().series_title.empty() ? children.front().title : children.front().series_title;
    display.series_title = display.title;
    display.part_title.clear();
    display.quality = 0;
    display.quality_desc.clear();
    display.video_codec_id = 0;
    display.video_bandwidth = 0;
    display.audio_id = 0;
    display.audio_desc.clear();
    display.audio_codec_id = 0;
    display.audio_bandwidth = 0;
    display.error_message.clear();
    display.downloaded_bytes = 0;
    display.total_bytes = 0;
    display.audio_downloaded_bytes = 0;
    display.audio_total_bytes = 0;
    display.estimated_bytes = 0;
    display.finished_at = 0;

    size_t completed = 0;
    const DownloadTask* active = nullptr;
    const DownloadTask* failed = nullptr;
    const DownloadTask* paused = nullptr;
    const DownloadTask* pending = nullptr;
    const DownloadTask* cancelled = nullptr;

    for (const auto& task : children) {
        if (display.cover_url.empty() && !task.cover_url.empty()) display.cover_url = task.cover_url;
        display.downloaded_bytes += std::max<int64_t>(0, task.downloaded_bytes);
        display.total_bytes += std::max<int64_t>(0, task.total_bytes);
        display.audio_downloaded_bytes += std::max<int64_t>(0, task.audio_downloaded_bytes);
        display.audio_total_bytes += std::max<int64_t>(0, task.audio_total_bytes);
        display.estimated_bytes += std::max<int64_t>(0, DownloadManager::estimateBytes(task));
        display.finished_at = std::max(display.finished_at, task.finished_at);

        switch (task.status) {
            case DownloadTaskStatus::COMPLETED: ++completed; break;
            case DownloadTaskStatus::DOWNLOADING: if (!active) active = &task; break;
            case DownloadTaskStatus::FAILED: if (!failed) failed = &task; break;
            case DownloadTaskStatus::PAUSED: if (!paused) paused = &task; break;
            case DownloadTaskStatus::PENDING: if (!pending) pending = &task; break;
            case DownloadTaskStatus::CANCELLED: if (!cancelled) cancelled = &task; break;
        }
    }

    display.owner_name = fmt::format(
        fmt::runtime("wiliwili/download_manager/collection_meta"_i18n),
        children.size(), completed, children.size());

    if (completed == children.size()) {
        display.status = DownloadTaskStatus::COMPLETED;
        display.stage = DownloadTaskStage::COMPLETED;
    } else if (active) {
        display.status = DownloadTaskStatus::DOWNLOADING;
        display.stage = active->stage;
    } else if (failed) {
        display.status = DownloadTaskStatus::FAILED;
        display.stage = failed->stage;
        display.error_message = failed->error_message;
    } else if (paused) {
        display.status = DownloadTaskStatus::PAUSED;
        display.stage = paused->stage;
    } else if (pending) {
        display.status = DownloadTaskStatus::PENDING;
        display.stage = pending->stage;
    } else {
        display.status = DownloadTaskStatus::CANCELLED;
        display.stage = cancelled ? cancelled->stage : DownloadTaskStage::QUEUED;
    }

    return display;
}

struct DownloadGridEntry {
    bool isCollection = false;
    std::string groupKey;
    DownloadTask display;
    std::vector<DownloadTask> children;
};

static std::vector<DownloadGridEntry> makeDownloadGridEntries(const std::vector<DownloadTask>& tasks) {
    std::unordered_map<std::string, size_t> counts;
    for (const auto& task : tasks) {
        const auto key = downloadTaskGroupKey(task);
        if (!key.empty()) ++counts[key];
    }

    std::unordered_map<std::string, bool> emitted;
    std::vector<DownloadGridEntry> entries;
    entries.reserve(tasks.size());

    for (const auto& task : tasks) {
        const auto key = downloadTaskGroupKey(task);
        const bool isCollection = !key.empty() && counts[key] > 1;
        if (!isCollection) {
            DownloadGridEntry entry;
            entry.display = task;
            entries.emplace_back(std::move(entry));
            continue;
        }

        if (emitted[key]) continue;
        emitted[key] = true;

        DownloadGridEntry entry;
        entry.isCollection = true;
        entry.groupKey = key;
        for (const auto& child : tasks) {
            if (downloadTaskGroupKey(child) == key) entry.children.push_back(child);
        }
        entry.display = makeCollectionDisplayTask(entry.children);
        entries.emplace_back(std::move(entry));
    }

    return entries;
}

static void showTaskActions(const DownloadTask& task, bool libraryMode) {
    auto* dialog = new brls::Dialog(task.title.empty() ? task.bvid : task.title);

    // Keep a visible way out at the very top. On handheld screens a long action list used to
    // push this button and destructive actions below the visible area.
    dialog->addButton("hints/cancel"_i18n, []() {});

    if (task.status == DownloadTaskStatus::COMPLETED) {
        dialog->addButton("wiliwili/download_manager/play"_i18n, [task]() { playOfflineTask(task); });
        dialog->addButton("wiliwili/download_manager/delete"_i18n, [id = task.id, title = task.title]() {
            auto* confirm = new brls::Dialog(
                "wiliwili/download_manager/delete_confirm"_i18n +
                (title.empty() ? "" : "\n" + title));
            confirm->addButton("hints/cancel"_i18n, []() {});
            confirm->addButton("wiliwili/download_manager/delete"_i18n,
                               [id]() { DownloadManager::instance().deleteTask(id); });
            confirm->open();
        });
    } else if (task.status == DownloadTaskStatus::DOWNLOADING) {
        dialog->addButton("wiliwili/download_manager/pause"_i18n,
                          [id = task.id]() { DownloadManager::instance().pauseTask(id); });
        dialog->addButton("wiliwili/download_manager/cancel"_i18n,
                          [id = task.id]() { DownloadManager::instance().cancelTask(id); });
    } else if (task.status == DownloadTaskStatus::PAUSED) {
        dialog->addButton("wiliwili/download_manager/resume"_i18n,
                          [id = task.id]() { DownloadManager::instance().resumeTask(id); });
        dialog->addButton("wiliwili/download_manager/cancel"_i18n,
                          [id = task.id]() { DownloadManager::instance().cancelTask(id); });
        dialog->addButton("wiliwili/download_manager/delete"_i18n, [id = task.id, title = task.title]() {
            auto* confirm = new brls::Dialog(
                "wiliwili/download_manager/delete_confirm"_i18n +
                (title.empty() ? "" : "\n" + title));
            confirm->addButton("hints/cancel"_i18n, []() {});
            confirm->addButton("wiliwili/download_manager/delete"_i18n,
                               [id]() { DownloadManager::instance().deleteTask(id); });
            confirm->open();
        });
    } else if (task.status == DownloadTaskStatus::FAILED || task.status == DownloadTaskStatus::CANCELLED) {
        dialog->addButton("wiliwili/download_manager/retry"_i18n,
                          [id = task.id]() { DownloadManager::instance().retryTask(id); });
        dialog->addButton("wiliwili/download_manager/delete"_i18n, [id = task.id, title = task.title]() {
            auto* confirm = new brls::Dialog(
                "wiliwili/download_manager/delete_confirm"_i18n +
                (title.empty() ? "" : "\n" + title));
            confirm->addButton("hints/cancel"_i18n, []() {});
            confirm->addButton("wiliwili/download_manager/delete"_i18n,
                               [id]() { DownloadManager::instance().deleteTask(id); });
            confirm->open();
        });
    } else if (task.status == DownloadTaskStatus::PENDING) {
        dialog->addButton("wiliwili/download_manager/cancel"_i18n,
                          [id = task.id]() { DownloadManager::instance().cancelTask(id); });
        dialog->addButton("wiliwili/download_manager/delete"_i18n, [id = task.id, title = task.title]() {
            auto* confirm = new brls::Dialog(
                "wiliwili/download_manager/delete_confirm"_i18n +
                (title.empty() ? "" : "\n" + title));
            confirm->addButton("hints/cancel"_i18n, []() {});
            confirm->addButton("wiliwili/download_manager/delete"_i18n,
                               [id]() { DownloadManager::instance().deleteTask(id); });
            confirm->open();
        });
    }

    if (!libraryMode && (task.status == DownloadTaskStatus::PENDING || task.status == DownloadTaskStatus::PAUSED)) {
        dialog->addButton("wiliwili/download_manager/move_up"_i18n,
                          [id = task.id]() { DownloadManager::instance().moveTaskUp(id); });
        dialog->addButton("wiliwili/download_manager/move_down"_i18n,
                          [id = task.id]() { DownloadManager::instance().moveTaskDown(id); });
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
        dialog->addButton("wiliwili/download_manager/show_error"_i18n,
                          [error = task.error_message]() { brls::Application::notify(error); });
    }
    dialog->open();
}

class DownloadTaskDataSource : public RecyclingGridDataSource {
public:
    DownloadTaskDataSource(std::vector<DownloadTask> tasks, bool libraryMode)
        : tasks(std::move(tasks)), libraryMode(libraryMode) {}

    size_t getItemCount() override { return tasks.size(); }

    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        auto* cell = static_cast<DownloadCard*>(recycler->dequeueReusableCell("Cell"));
        cell->setTask(tasks[index]);
        return cell;
    }

    void onItemSelected(RecyclingGrid*, size_t index) override {
        if (index >= tasks.size()) return;
        showTaskActions(tasks[index], libraryMode);
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

class DownloadOverviewDataSource : public RecyclingGridDataSource {
public:
    DownloadOverviewDataSource(std::vector<DownloadGridEntry> entries, bool libraryMode)
        : entries(std::move(entries)), libraryMode(libraryMode) {}

    size_t getItemCount() override { return entries.size(); }

    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        auto* cell = static_cast<DownloadCard*>(recycler->dequeueReusableCell("Cell"));
        cell->setTask(entries[index].display);
        return cell;
    }

    void onItemSelected(RecyclingGrid*, size_t index) override {
        if (index >= entries.size()) return;
        const auto& entry = entries[index];
        if (entry.isCollection) {
            brls::Application::pushActivity(
                new DownloadCollectionActivity(entry.groupKey, entry.display.title, libraryMode));
            return;
        }
        showTaskActions(entry.display, libraryMode);
    }

    void clearData() override { entries.clear(); }

    bool updateTask(const DownloadTask& task, size_t& index) {
        for (size_t i = 0; i < entries.size(); ++i) {
            auto& entry = entries[i];
            if (!entry.isCollection) {
                if (entry.display.id != task.id) continue;
                entry.display = task;
                index = i;
                return true;
            }

            for (auto& child : entry.children) {
                if (child.id != task.id) continue;
                child = task;
                entry.display = makeCollectionDisplayTask(entry.children);
                index = i;
                return true;
            }
        }
        return false;
    }

private:
    std::vector<DownloadGridEntry> entries;
    bool libraryMode = false;
};

void DownloadActivity::onContentAvailable() {
    grid->registerCell("Cell", []() { return DownloadCard::create(); });
    grid->estimatedRowHeight = 250;
    reload();
    registerAction("wiliwili/download_manager/offline_library"_i18n, brls::ControllerButton::BUTTON_Y,
                   [](brls::View*) { brls::Application::pushActivity(new OfflineLibraryActivity()); return true; }, true);
    progressSub = DownloadManager::instance().getTaskProgressEvent()->subscribe([this](const std::string& id) {
        // Progress arrives several times per second. Replacing the entire data source here resets
        // scroll/focus state and feels especially bad with a controller. Update only the matching
        // visible card; status/queue changes still use reload() because they can reorder rows.
        DownloadTask task;
        if (DownloadManager::instance().getTaskSnapshot(id, task)) {
            auto* dataSource = dynamic_cast<DownloadOverviewDataSource*>(grid->getDataSource());
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
    for (const auto& task : tasks) DownloadManager::instance().ensureTaskCover(task);
    // Preserve queue order for active/pending tasks so move-up/down is meaningful; completed items
    // are grouped below the queue by completion time.
    std::stable_sort(tasks.begin(), tasks.end(), [](const DownloadTask& a, const DownloadTask& b) {
        const bool ac = a.status == DownloadTaskStatus::COMPLETED;
        const bool bc = b.status == DownloadTaskStatus::COMPLETED;
        if (ac != bc) return !ac;
        if (ac && bc) return a.finished_at > b.finished_at;
        return false;
    });
    grid->setDataSource(new DownloadOverviewDataSource(makeDownloadGridEntries(tasks), false));
    if (grid->getItemCount() == 0) grid->setEmpty("wiliwili/download_manager/empty"_i18n);
}

DownloadActivity::~DownloadActivity() {
    DownloadManager::instance().getTaskProgressEvent()->unsubscribe(progressSub);
    DownloadManager::instance().getTaskStatusChangedEvent()->unsubscribe(statusSub);
}


void DownloadCollectionActivity::onContentAvailable() {
    titleLabel->setText(groupTitle);
    grid->registerCell("Cell", []() { return DownloadCard::create(); });
    grid->estimatedRowHeight = 250;
    reload();

    progressSub = DownloadManager::instance().getTaskProgressEvent()->subscribe([this](const std::string& id) {
        DownloadTask task;
        if (!DownloadManager::instance().getTaskSnapshot(id, task)) return;
        if (downloadTaskGroupKey(task) != groupKey) return;
        if (completedOnly && task.status != DownloadTaskStatus::COMPLETED) return;

        auto* dataSource = dynamic_cast<DownloadTaskDataSource*>(grid->getDataSource());
        size_t index = 0;
        if (dataSource && dataSource->updateTask(task, index)) {
            auto* card = dynamic_cast<DownloadCard*>(grid->getGridItemByIndex(index));
            if (card) card->setTask(task);
        }
    });

    statusSub = DownloadManager::instance().getTaskStatusChangedEvent()->subscribe(
        [this](const std::string&) { reload(); });
}

void DownloadCollectionActivity::reload() {
    auto all = DownloadManager::instance().getTasksSnapshot();
    std::vector<DownloadTask> children;
    for (const auto& task : all) {
        if (downloadTaskGroupKey(task) != groupKey) continue;
        if (completedOnly && task.status != DownloadTaskStatus::COMPLETED) continue;
        DownloadManager::instance().ensureTaskCover(task);
        children.push_back(task);
    }

    std::stable_sort(children.begin(), children.end(), [](const DownloadTask& a, const DownloadTask& b) {
        if (a.part_index > 0 && b.part_index > 0 && a.part_index != b.part_index)
            return a.part_index < b.part_index;
        if (a.created_at != b.created_at) return a.created_at < b.created_at;
        return a.title < b.title;
    });

    grid->setDataSource(new DownloadTaskDataSource(std::move(children), completedOnly));
    if (grid->getItemCount() == 0)
        grid->setEmpty(completedOnly
            ? "wiliwili/download_manager/library_empty"_i18n
            : "wiliwili/download_manager/empty"_i18n);
}

DownloadCollectionActivity::~DownloadCollectionActivity() {
    DownloadManager::instance().getTaskProgressEvent()->unsubscribe(progressSub);
    DownloadManager::instance().getTaskStatusChangedEvent()->unsubscribe(statusSub);
}

void OfflineLibraryActivity::onContentAvailable() {
    grid->registerCell("Cell", []() { return DownloadCard::create(); });
    grid->estimatedRowHeight = 250;
    reload();
    statusSub = DownloadManager::instance().getTaskStatusChangedEvent()->subscribe([this](const std::string&) { reload(); });
}

void OfflineLibraryActivity::reload() {
    auto all = DownloadManager::instance().getTasksSnapshot();
    for (const auto& task : all) DownloadManager::instance().ensureTaskCover(task);
    std::vector<DownloadTask> completed;
    for (auto& task : all) if (task.status == DownloadTaskStatus::COMPLETED) completed.emplace_back(std::move(task));
    std::stable_sort(completed.begin(), completed.end(), [](const auto& a, const auto& b) { return a.finished_at > b.finished_at; });
    grid->setDataSource(new DownloadOverviewDataSource(makeDownloadGridEntries(completed), true));
    if (grid->getItemCount() == 0) grid->setEmpty("wiliwili/download_manager/library_empty"_i18n);
}

OfflineLibraryActivity::~OfflineLibraryActivity() {
    DownloadManager::instance().getTaskStatusChangedEvent()->unsubscribe(statusSub);
}

OfflinePlayerActivity::~OfflinePlayerActivity() {
    if (mpvEventSubscribed) {
        MPV_E->unsubscribe(mpvEventSubscription);
        mpvEventSubscribed = false;
    }

    video->stop();

    // Local playback is reliability-first and temporarily uses software decoding. Restore the
    // user's configured online hwdec mode immediately when leaving the offline player.
    if (offlineHwdecOverridden && MPVCore::HARDWARE_DEC) {
        MPVCore::instance().command_async("set", "hwdec", MPVCore::PLAYER_HWDEC_METHOD);
        brls::Logger::info("OfflinePlayer: restore hwdec={}", MPVCore::PLAYER_HWDEC_METHOD);
        offlineHwdecOverridden = false;
    }
}

void OfflinePlayerActivity::onContentAvailable() {
    // Reuse the exact full-screen VideoView resource used by DLNA/live playback instead of
    // maintaining a second custom player layout/render path.
    video->hideDLNAButton();
    video->hideDanmakuButton();
    video->hideVideoQualityButton();
    video->hideSubtitleSetting();
    video->hideVideoRelatedSetting();
    video->hideHistorySetting();
    video->hideHighlightLineSetting();
    video->hideSkipOpeningCreditsSetting();
    video->disableCloseOnEndOfFile();
    video->setFullscreenIcon(true);
    video->setTitle(task.title);
    video->showOSD(false);
    video->registerCommonActions(this);

    this->registerAction(
        "cancel", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            if (video->isOSDLock())
                video->toggleOSD();
            else
                brls::Application::popActivity();
            return true;
        },
        true);

    const auto videoPath = DownloadManager::playableVideoPath(task);
    const auto audioPath = DownloadManager::playableAudioPath(task);
    if (videoPath.empty()) {
        brls::Application::notify("wiliwili/download_manager/play_missing"_i18n);
        return;
    }

    subtitlePath = selectOfflineSubtitle(task);
    subtitleAttached = false;

    auto& mpv = MPVCore::instance();
    mpv.reset();
    mpv.setAspect(
        ProgramConfig::instance().getSettingItem(SettingItem::PLAYER_ASPECT, std::string{"-1"}));

    // Online playback proves the OpenGL renderer works on this SteamOS/Legion Go combination.
    // The remaining grey-frame failure was local-media specific and survived auto-copy, so remove
    // VAAPI/hwdec interop from the local path completely. The Z1 Extreme can software-decode the
    // common 1080p AVC/HEVC/AV1 offline workload; online playback keeps the user's normal hwdec.
    if (MPVCore::HARDWARE_DEC) {
        mpv.command_async("set", "hwdec", "no");
        offlineHwdecOverridden = true;
        brls::Logger::info("OfflinePlayer: temporarily disable hwdec for local playback");
    }

    mpvEventSubscription = MPV_E->subscribe([this](MpvEventEnum event) {
        if (event == MpvEventEnum::MPV_LOADED) {
            if (!subtitleAttached && !subtitlePath.empty()) {
                MPVCore::instance().command_async("sub-add", subtitlePath, "select");
                subtitleAttached = true;
            }
            video->resume();
        } else if (event == MpvEventEnum::MPV_FILE_ERROR) {
            brls::Application::notify("wiliwili/download_manager/play_missing"_i18n);
        }
    });
    mpvEventSubscribed = true;

    brls::Logger::info(
        "OfflinePlayer: video={}, external_audio={}, subtitle={}, hwdec=no",
        cpr::fs::path(videoPath).filename().string(),
        !audioPath.empty(),
        !subtitlePath.empty());

    // Use VideoView::setUrl(), the same established loading path as normal online playback.
    if (audioPath.empty())
        video->setUrl(videoPath, 0, 0);
    else
        video->setUrl(videoPath, 0, 0, audioPath);

    video->resume();
    brls::Application::giveFocus(video);
}

