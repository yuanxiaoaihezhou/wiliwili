#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <borealis/core/activity.hpp>
#include <borealis/core/bind.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/image.hpp>

#include "view/text_box.hpp"

#include "utils/download_manager.hpp"
#include "utils/event_helper.hpp"
#include "view/recycling_grid.hpp"

class VideoView;

class DownloadCard : public RecyclingGridItem {
public:
    DownloadCard();
    ~DownloadCard() override;
    static RecyclingGridItem* create();
    void setTask(const DownloadTask& task);
    void prepareForReuse() override;
    void cacheForReuse() override;

private:
    BRLS_BIND(brls::Image, coverImage, "download/cover");
    BRLS_BIND(brls::Box, statusBox, "download/status_box");
    BRLS_BIND(brls::Label, statusLabel, "download/status");
    BRLS_BIND(brls::Label, progressLabel, "download/progress");
    BRLS_BIND(brls::Label, percentLabel, "download/percent");
    BRLS_BIND(brls::Box, progressBar, "download/progress_bar");
    BRLS_BIND(TextBox, titleLabel, "download/title");
    BRLS_BIND(brls::Label, metaLabel, "download/meta");
    std::string loadedCoverUrl;
};

class DownloadActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/download_activity.xml");
    DownloadActivity() = default;
    ~DownloadActivity() override;
    void onContentAvailable() override;

private:
    BRLS_BIND(RecyclingGrid, grid, "download/grid");
    BRLS_BIND(brls::Label, summaryLabel, "download/summary");
    brls::Event<std::string>::Subscription progressSub;
    brls::Event<std::string>::Subscription statusSub;
    std::chrono::steady_clock::time_point lastProgressRefresh{};
    void reload();
};

class DownloadCollectionActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/download_collection_activity.xml");
    DownloadCollectionActivity(std::string groupKey, std::string groupTitle, bool completedOnly)
        : groupKey(std::move(groupKey)), groupTitle(std::move(groupTitle)), completedOnly(completedOnly) {}
    ~DownloadCollectionActivity() override;
    void onContentAvailable() override;

private:
    std::string groupKey;
    std::string groupTitle;
    bool completedOnly = false;
    BRLS_BIND(RecyclingGrid, grid, "download/grid");
    BRLS_BIND(brls::Label, titleLabel, "download/title");
    brls::Event<std::string>::Subscription progressSub;
    brls::Event<std::string>::Subscription statusSub;
    void reload();
};

class OfflineLibraryActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/offline_library_activity.xml");
    OfflineLibraryActivity() = default;
    ~OfflineLibraryActivity() override;
    void onContentAvailable() override;

private:
    BRLS_BIND(RecyclingGrid, grid, "download/grid");
    brls::Event<std::string>::Subscription statusSub;
    void reload();
};

class OfflinePlayerActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/video_activity.xml");
    explicit OfflinePlayerActivity(DownloadTask task) : task(std::move(task)) {}
    ~OfflinePlayerActivity() override;
    void onContentAvailable() override;

private:
    DownloadTask task;
    std::string subtitlePath;
    MPVEvent::Subscription mpvEventSubscription{};
    bool mpvEventSubscribed = false;
    bool offlineHwdecOverridden = false;
    bool subtitleAttached = false;
    BRLS_BIND(VideoView, video, "video");
};
