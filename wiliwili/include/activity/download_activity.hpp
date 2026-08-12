#pragma once

#include <chrono>
#include <borealis/core/activity.hpp>
#include <borealis/core/bind.hpp>
#include <borealis/views/label.hpp>

#include "utils/download_manager.hpp"
#include "view/recycling_grid.hpp"

class DownloadCard : public RecyclingGridItem {
public:
    DownloadCard();
    static RecyclingGridItem* create();
    void setTask(const DownloadTask& task);

private:
    BRLS_BIND(brls::Label, titleLabel, "download/title");
    BRLS_BIND(brls::Label, metaLabel, "download/meta");
    BRLS_BIND(brls::Label, progressLabel, "download/progress");
    BRLS_BIND(brls::Label, statusLabel, "download/status");
};

class DownloadActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/download_activity.xml");
    DownloadActivity() = default;
    ~DownloadActivity() override;
    void onContentAvailable() override;

private:
    BRLS_BIND(RecyclingGrid, grid, "download/grid");
    brls::Event<std::string>::Subscription progressSub;
    brls::Event<std::string>::Subscription statusSub;
    std::chrono::steady_clock::time_point lastProgressRefresh{};
    void reload();
};
