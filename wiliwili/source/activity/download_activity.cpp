#include "activity/download_activity.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <fmt/format.h>

#include <borealis/core/application.hpp>
#include <borealis/views/dialog.hpp>
#include "utils/dialog_helper.hpp"

using namespace brls::literals;

static std::string humanBytes(int64_t value) {
    if (value <= 0) return "0 B";
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double n = static_cast<double>(value); int unit = 0;
    while (n >= 1024.0 && unit < 4) { n /= 1024.0; ++unit; }
    std::ostringstream ss; ss << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << n << " " << units[unit];
    return ss.str();
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

DownloadCard::DownloadCard() { inflateFromXMLRes("xml/views/download_card.xml"); }
RecyclingGridItem* DownloadCard::create() { return new DownloadCard(); }

void DownloadCard::setTask(const DownloadTask& task) {
    titleLabel->setText(task.title.empty() ? task.bvid : task.title);
    std::string meta = task.quality_desc.empty() ? std::to_string(task.quality) : task.quality_desc;
    if (!task.audio_desc.empty()) meta += "  ·  " + task.audio_desc;
    if (!task.owner_name.empty()) meta += "  ·  " + task.owner_name;
    metaLabel->setText(meta);

    int64_t done = task.downloaded_bytes + task.audio_downloaded_bytes;
    int64_t total = task.total_bytes + task.audio_total_bytes;
    std::string progress = humanBytes(done);
    if (total > 0) {
        int percent = static_cast<int>(std::min<int64_t>(100, done * 100 / std::max<int64_t>(1, total)));
        progress = fmt::format("{} / {}  ·  {}%", humanBytes(done), humanBytes(total), percent);
    }
    if (task.status == DownloadTaskStatus::COMPLETED) {
        if (task.muxed && !task.output_file.empty()) progress = task.output_file + "  ·  " + humanBytes(done);
        else progress = task.video_file + (task.audio_file.empty() ? "" : " + " + task.audio_file) + "  ·  " + humanBytes(done);
    }
    progressLabel->setText(progress);
    statusLabel->setText(statusText(task.status));
}

class DownloadDataSource : public RecyclingGridDataSource {
public:
    explicit DownloadDataSource(std::vector<DownloadTask> tasks) : tasks(std::move(tasks)) {}

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
        if (task.status == DownloadTaskStatus::DOWNLOADING) {
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
        if (!task.dir.empty()) {
            dialog->addButton("wiliwili/download_manager/show_path"_i18n, [path = task.dir]() {
                // Gaming-mode shells do not have a reliable file-manager URI handler; showing the
                // exact path is deterministic on SteamOS, desktop Linux and the other platforms.
                brls::Application::notify(path);
            });
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

private:
    std::vector<DownloadTask> tasks;
};

void DownloadActivity::onContentAvailable() {
    grid->registerCell("Cell", []() { return DownloadCard::create(); });
    grid->estimatedRowHeight = 118;
    reload();
    progressSub = DownloadManager::instance().getTaskProgressEvent()->subscribe([this](const std::string&) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastProgressRefresh).count() >= 0.5) {
            lastProgressRefresh = now;
            reload();
        }
    });
    statusSub = DownloadManager::instance().getTaskStatusChangedEvent()->subscribe([this](const std::string&) { reload(); });
}

void DownloadActivity::reload() {
    auto tasks = DownloadManager::instance().getTasksSnapshot();
    std::stable_sort(tasks.begin(), tasks.end(), [](const DownloadTask& a, const DownloadTask& b) {
        if (a.status == DownloadTaskStatus::COMPLETED && b.status != DownloadTaskStatus::COMPLETED) return false;
        if (a.status != DownloadTaskStatus::COMPLETED && b.status == DownloadTaskStatus::COMPLETED) return true;
        return a.created_at > b.created_at;
    });
    grid->setDataSource(new DownloadDataSource(std::move(tasks)));
    if (grid->getItemCount() == 0) grid->setEmpty("wiliwili/download_manager/empty"_i18n);
}

DownloadActivity::~DownloadActivity() {
    DownloadManager::instance().getTaskProgressEvent()->unsubscribe(progressSub);
    DownloadManager::instance().getTaskStatusChangedEvent()->unsubscribe(statusSub);
}
