//
// Created by fang on 2023/1/3.
//

#include <utility>
#include <algorithm>
#include <map>
#include <set>
#include <functional>
#include <iomanip>
#include <sstream>
#include <borealis/core/thread.hpp>
#include <borealis/core/touch/tap_gesture.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/dialog.hpp>

#include "activity/player_activity.hpp"
#include "fragment/player_collection.hpp"
#include "fragment/player_coin.hpp"
#include "fragment/player_single_comment.hpp"
#include "utils/config_helper.hpp"
#include "utils/download_manager.hpp"
#include "utils/dialog_helper.hpp"
#include "utils/number_helper.hpp"
#include "presenter/comment_related.hpp"
#include "utils/shortcut_helper.hpp"
#include "view/qr_image.hpp"
#include "view/video_view.hpp"
#include "view/grid_dropdown.hpp"
#include "view/subtitle_core.hpp"
#include "view/mpv_core.hpp"

class DataSourceCommentList : public RecyclingGridDataSource, public CommentAction {
public:
    DataSourceCommentList(bilibili::VideoCommentListResult result, uint64_t aid, int mode, std::function<void(void)> cb)
        : dataList(std::move(result)), aid(aid), commentMode(mode), switchModeCallback(cb) {}
    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        if (index == 0) {
            VideoCommentSort* item = (VideoCommentSort*)recycler->dequeueReusableCell("Sort");
            item->setHeight(30);
            item->setFocusable(false);
            if (commentMode == 3) {
                item->hintLabel->setText("wiliwili/player/comment_sort/top"_i18n);
                item->sortLabel->setText("wiliwili/player/comment_sort/sort_top"_i18n);
            } else {
                item->hintLabel->setText("wiliwili/player/comment_sort/new"_i18n);
                item->sortLabel->setText("wiliwili/player/comment_sort/sort_new"_i18n);
            }
            return item;
        }
        if (index == 1) {
            VideoCommentReply* item = (VideoCommentReply*)recycler->dequeueReusableCell("Reply");
            item->setHeight(40);
            return item;
        }

        //从缓存列表中取出 或者 新生成一个表单项
        VideoComment* item = (VideoComment*)recycler->dequeueReusableCell("Cell");

        item->setData(this->dataList[index - 2]);
        return item;
    }

    size_t getItemCount() override { return dataList.size() + 2; }

    void onItemSelected(RecyclingGrid* recycler, size_t index) override {
        if (index == 0) {
            if (switchModeCallback) switchModeCallback();
            return;
        }
        if (index == 1) {
            if (!DialogHelper::checkLogin()) return;
            // 回复评论
            brls::Application::getImeManager()->openForText(
                [this, recycler](const std::string& text) {
                    if (text.empty()) return;
                    this->commentReply(text, std::to_string(aid), 0, 0, 1,
                                       [this, recycler](const bilibili::VideoCommentAddResult& result) {
                                           this->dataList.insert(dataList.begin(), result.reply);
                                           recycler->reloadData();
                                       });
                },
                "wiliwili/player/single_comment/hint"_i18n, "", 500, "", 0);
            return;
        }

        auto* item = dynamic_cast<VideoComment*>(recycler->getGridItemByIndex(index));
        if (!item) return;

        auto* view = new PlayerSingleComment();
        view->setCommentData(dataList[index - 2], item->getY(), 1);
        auto container = new brls::AppletFrame(view);
        container->setHeaderVisibility(brls::Visibility::GONE);
        container->setFooterVisibility(brls::Visibility::GONE);
        container->setInFadeAnimation(true);
        brls::Application::pushActivity(new brls::Activity(container));

        view->likeStateEvent.subscribe([this, item, index](size_t value) {
            auto& itemData  = dataList[index - 2];
            itemData.action = value;
            item->setLiked(value);
        });
        view->likeNumEvent.subscribe([this, item, index](size_t value) {
            auto& itemData = dataList[index - 2];
            itemData.like  = value;
            item->setLikeNum(value);
        });
        view->replyNumEvent.subscribe([this, item, index](size_t value) {
            auto& itemData  = dataList[index - 2];
            itemData.rcount = value;
            item->setReplyNum(value);
        });
        view->deleteEvent.subscribe([this, recycler, index]() {
            dataList.erase(dataList.begin() + index - 2);
            recycler->reloadData();
            // 重新设置一下焦点到 recycler 的默认 cell （顶部）
            brls::Application::giveFocus(recycler);
        });
    }

    void appendData(const bilibili::VideoCommentListResult& data) {
        bool skip = false;
        for (auto& i : data) {
            skip = false;
            for (auto& j : this->dataList) {
                if (j.rpid == i.rpid) {
                    skip = true;
                    break;
                }
            }
            if (!skip) this->dataList.push_back(i);
        }
    }

    void clearData() override { this->dataList.clear(); }

private:
    bilibili::VideoCommentListResult dataList;
    uint64_t aid;
    int commentMode                              = 3;  // 2: 按时间；3: 按热度
    std::function<void(void)> switchModeCallback = nullptr;
};

class QualityCell : public RecyclingGridItem {
public:
    QualityCell() { this->inflateFromXMLRes("xml/views/player_quality_cell.xml"); }

    void setSelected(bool selected) {
        brls::Theme theme = brls::Application::getTheme();

        this->selected = selected;
        this->checkbox->setVisibility(selected ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        this->title->setTextColor(selected ? theme["brls/list/listItem_value_color"] : theme["brls/text"]);
    }

    bool getSelected() { return this->selected; }

    BRLS_BIND(brls::Label, title, "cell/title");
    BRLS_BIND(brls::Box, loginLabel, "cell/login");
    BRLS_BIND(brls::Box, vipLabel, "cell/vip");
    BRLS_BIND(brls::CheckBox, checkbox, "cell/checkbox");

    static RecyclingGridItem* create() { return new GridRadioCell(); }

private:
    bool selected = false;
};

class QualityDataSource : public DataSourceDropdown {
public:
    QualityDataSource(bilibili::VideoUrlResult result, BaseDropdown* view)
        : DataSourceDropdown(view), data(std::move(result)) {
        login = !ProgramConfig::instance().getCSRF().empty();
    }

    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        QualityCell* item = (QualityCell*)recycler->dequeueReusableCell("Cell");

        int quality = data.accept_quality[index];

        item->loginLabel->setVisibility(brls::Visibility::GONE);
        item->vipLabel->setVisibility(brls::Visibility::GONE);

        if (quality > 80) {
            item->vipLabel->setVisibility(brls::Visibility::VISIBLE);
        } else if (quality >= 32) {
            if (!login) item->loginLabel->setVisibility(brls::Visibility::VISIBLE);
        }

        auto r = this->data.accept_description[index];
        item->title->setText(this->data.accept_description[index]);
        item->setSelected(index == dropdown->getSelected());
        return item;
    }

    size_t getItemCount() override { return (std::min)(data.accept_quality.size(), data.accept_description.size()); }

    void clearData() override {}

private:
    bilibili::VideoUrlResult data;
    bool login;
};

/// BasePlayerActivity

void BasePlayerActivity::onContentAvailable() { this->setCommonData(); }

void BasePlayerActivity::setCommonData() {
    // 视频评论
    recyclingGrid->registerCell("Cell", []() { return VideoComment::create(); });

    recyclingGrid->registerCell("Reply", []() { return VideoCommentReply::create(); });

    recyclingGrid->registerCell("Sort", []() { return VideoCommentSort::create(); });

    recyclingGrid->setDefaultCellFocus(1);

    recyclingGrid->registerAction("wiliwili/home/common/switch"_i18n, brls::ControllerButton::BUTTON_X,
                                  [this](brls::View* view) -> bool {
                                      this->setCommentMode();
                                      return true;
                                  });

    recyclingGrid->registerAction(ShortcutHelper::getRefresh(),
                                  [this](brls::View* view) -> bool {
                                      this->setCommentMode();
                                      return true;
                                  });

    // 切换右侧Tab
    this->registerAction(
        "上一项", brls::ControllerButton::BUTTON_LT,
        [this](brls::View* view) -> bool {
            tabFrame->focus2LastTab();
            return true;
        },
        true);
    this->registerAction(
        "上一项", brls::ControllerButton::BUTTON_LB,
        [this](brls::View* view) -> bool {
            tabFrame->focus2LastTab();
            return true;
        },
        true);

    this->registerAction(
        "下一项", brls::ControllerButton::BUTTON_RT,
        [this](brls::View* view) -> bool {
            tabFrame->focus2NextTab();
            return true;
        },
        true);
    this->registerAction(
        "下一项", brls::ControllerButton::BUTTON_RB,
        [this](brls::View* view) -> bool {
            tabFrame->focus2NextTab();
            return true;
        },
        true);

    this->registerAction(
        ShortcutHelper::getLast(),
        [this](brls::View* view) -> bool {
            tabFrame->focus2LastTab();
            return true;
        });
    this->registerAction(
        ShortcutHelper::getNext(),
        [this](brls::View* view) -> bool {
            tabFrame->focus2NextTab();
            return true;
        });
    video->registerCommonActions(this);

    // 调整清晰度
    this->registerAction("wiliwili/player/quality"_i18n, brls::ControllerButton::BUTTON_START,
                         [this](brls::View* view) -> bool {
                             this->setVideoQuality();
                             return true;
                         });

    this->btnQR->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnQR->getParent()));

    this->btnAgree->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnAgree->getParent()));

    this->btnCoin->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnCoin->getParent()));

    this->btnFavorite->getParent()->addGestureRecognizer(
        new brls::TapGestureRecognizer(this->btnFavorite->getParent()));

    this->btnDownload->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnDownload->getParent()));
    this->btnDownload->getParent()->registerClickAction([this](...) {
        this->showDownloadDialog();
        return true;
    });

    const auto uiProfile = ProgramConfig::instance().getSettingItem(SettingItem::APP_UI_PROFILE, std::string{"auto"});
    if (uiProfile == "handheld") {
        this->registerAction("wiliwili/player/download/label"_i18n, brls::ControllerButton::BUTTON_Y,
                             [this](brls::View*) -> bool { this->showDownloadDialog(); return true; }, true);
    }

    this->videoUserInfo->addGestureRecognizer(new brls::TapGestureRecognizer(this->videoUserInfo));

    this->setRelationButton(false, false, false);

    eventSubscribeID = MPV_E->subscribe([this](MpvEventEnum event) {
        // 上一次报告历史记录的时间点
        static int64_t lastProgress = MPVCore::instance().video_progress;
        switch (event) {
            case MpvEventEnum::UPDATE_PROGRESS: {
                // 每15秒同步一次进度
                if (lastProgress + 15 < MPVCore::instance().video_progress) {
                    lastProgress = MPVCore::instance().video_progress;
                    this->reportCurrentProgress(lastProgress, MPVCore::instance().duration);
                } else if (MPVCore::instance().video_progress < lastProgress) {
                    // 当前播放时间小于上一次上传历史记录的时间点
                    // 发生于向前拖拽进度的时候，此时重置lastProgress的值
                    lastProgress = MPVCore::instance().video_progress;
                }
                // 检查视频链接是否有效
                auto timeNow = std::chrono::system_clock::now();
                if (timeNow > videoDeadline) {
                    // 设置视频加载后跳转的时间
                    setProgress(MPVCore::instance().video_progress);

                    // 暂停播放
                    MPVCore::instance().pause();

                    // 10s 后重新尝试
                    videoDeadline = timeNow + std::chrono::seconds(10);

                    // 有效期已过，重新请求视频链接
                    auto self = dynamic_cast<PlayerSeasonActivity*>(this);
                    if (self) {
                        this->requestSeasonVideoUrl(episodeResult.bvid, episodeResult.cid, false);
                    } else {
                        this->requestVideoUrl(videoDetailResult.bvid, videoDetailPage.cid, false);
                    }

                    //todo: 如果有选择的字幕加载对应的字幕
                }
                break;
            }
            case MpvEventEnum::END_OF_FILE:
                // 尝试自动加载下一分集
                // 如果当前最顶层是Dialog就放弃自动播放，因为有可能是用户点开了收藏或者投币对话框
                {
                    int64_t& progress = MPVCore::instance().video_progress;
                    int64_t& duration = MPVCore::instance().duration;
                    int clipEnd       = videoUrlResult.clipEnd;
                    auto seasonCustom = ProgramConfig::instance().getSeasonCustom(seasonInfo.season_id);
                    if (seasonCustom.custom_clip) {
                        clipEnd = duration - seasonCustom.clip_end;
                    }

                    // 播放到一半没网时也会触发EOF，这里简单判断一下结束播放时的播放条位置是否在片尾或视频结尾附近
                    if ((duration - progress > 5 || progress - duration > 5) && !(clipEnd > 0 && clipEnd - progress < 5)) {
                        brls::Logger::error("EOF: video: {} duration: {} clipEnd: {}", progress, duration, clipEnd);
                        return;
                    }
                    if (PLAYER_STRATEGY == PlayerStrategy::LOOP) {
                        MPVCore::instance().seek(0);
                        MPVCore::instance().resume();
                        return;
                    }
                    auto stack    = brls::Application::getActivitiesStack();
                    Activity* top = stack[stack.size() - 1];
                    if (!dynamic_cast<BasePlayerActivity*>(top) &&
                        !dynamic_cast<VideoView*>(top->getContentView()->getView("video"))) {
                        // 最顶层没有 video 组件，说明用户打开了评论或者其他菜单
                        // 在这种情况下不执行自动播放其他视频显示重播按钮
                        APP_E->fire(VideoView::REPLAY, nullptr);
                    } else if (PLAYER_STRATEGY == PlayerStrategy::NEXT || PLAYER_STRATEGY == PlayerStrategy::RCMD) {
                        this->onIndexChangeToNext();
                    } else {
                        // 对于其他情况，显示重播按钮
                        APP_E->fire(VideoView::REPLAY, nullptr);
                    }
                }
                break;
            case MpvEventEnum::RESTART:
                this->updateVideoLink();
                break;
            default:
                break;
        }
    });

    customEventSubscribeID = APP_E->subscribe([this](const std::string& event, void* data) {
        if (event == VideoView::QUALITY_CHANGE) {
            this->setVideoQuality();
        } else if (event == VideoView::SWITCH_TO_LAST) {
            // 历史播放进度储存在 SubtitleCore 中
            auto videoPage = SubtitleCore::instance().getSubtitleList();
            if (videoPage.last_play_cid == videoDetailPage.cid) {
                // 因为占用了切换弹幕的按键，所以在无效的情况下保持切换弹幕
                this->video->toggleDanmaku();
                return;
            }
            brls::Logger::debug("切换到历史播放进度：{}/{}", videoPage.last_play_cid, videoPage.last_play_time);
            for (auto& p : videoDetailResult.pages) {
                if (p.cid == videoPage.last_play_cid) {
                    this->onIndexChange(p.page - 1);
                    this->setProgress(videoPage.last_play_time / 1000);
                    break;
                }
            }
        } else if (event == "REQUEST_CAST_URL") {
            this->requestCastUrl();
        }
    });

    if (brls::Application::ORIGINAL_WINDOW_HEIGHT < 720) video->hideStatusLabel();

    video->hideOSDLockButton();
}


namespace {
std::string downloadQualityDesc(const bilibili::VideoUrlResult& result, int quality) {
    for (size_t i = 0; i < result.accept_quality.size() && i < result.accept_description.size(); ++i) {
        if (result.accept_quality[i] == quality) return result.accept_description[i];
    }
    return std::to_string(quality);
}

std::vector<std::string> mediaUrls(const bilibili::DashMedia& media) {
    std::vector<std::string> urls;
    if (!media.base_url.empty()) urls.push_back(media.base_url);
    urls.insert(urls.end(), media.backup_url.begin(), media.backup_url.end());
    return urls;
}

std::string audioLabel(const bilibili::DashMedia& media, const std::string& kind = "") {
    std::string base;
    if (!kind.empty()) base = kind;
    else if (media.id == 30280) base = "192K";
    else if (media.id == 30232) base = "132K";
    else if (media.id == 30216) base = "64K";
    else base = fmt::format("Audio {}", media.id);
    if (media.bandwidth > 0) base += fmt::format(" · {} kbps", media.bandwidth / 1000);
    return base;
}

BaseDropdown* makeDownloadDropdown(const std::string& title, const std::vector<std::string>& values,
                                   ValueSelectedEvent::Callback callback) {
    auto* dropdown = new BaseDropdown(title, std::move(callback), 0);
    dropdown->getRecyclingList()->registerCell("Cell", []() {
        auto* cell = new GridRadioCell();
        cell->setHeight(brls::Application::getStyle()["brls/dropdown/listItemHeight"]);
        cell->title->setFontSize(brls::Application::getStyle()["brls/dropdown/listItemTextSize"]);
        return cell;
    });
    dropdown->setDataSource(new TextDataSourceDropdown(values, dropdown));
    return dropdown;
}

std::string videoCodecLabel(int codecid) {
    switch (codecid) {
        case 7: return "AVC / H.264";
        case 12: return "HEVC / H.265";
        case 13: return "AV1";
        default: return fmt::format("Codec {}", codecid);
    }
}

std::string humanDownloadBytes(int64_t value) {
    if (value <= 0) return "?";
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double n = static_cast<double>(value);
    int unit = 0;
    while (n >= 1024.0 && unit < 4) {
        n /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << n << ' ' << units[unit];
    return out.str();
}

int64_t estimateMediaBytes(const bilibili::DashMedia& video, const bilibili::DashMedia* audio, int duration) {
    if (duration <= 0) return 0;
    const uint64_t bandwidth = static_cast<uint64_t>(video.bandwidth) + (audio ? static_cast<uint64_t>(audio->bandwidth) : 0ULL);
    if (bandwidth == 0) return 0;
    return static_cast<int64_t>((bandwidth / 8.0) * duration * 1.04);
}

using DownloadSelectionCallback = std::function<void(DownloadTask)>;

bool showDownloadAudioForVideo(DownloadTask base, const bilibili::VideoUrlResult& result,
                               const bilibili::DashMedia& videoMedia, int quality,
                               DownloadSelectionCallback onSelected) {
    struct AudioChoice { bilibili::DashMedia media; std::string label; };
    std::vector<AudioChoice> choices;
    std::set<std::string> unique;
    auto append = [&](const bilibili::DashMedia& media, const std::string& kind = "") {
        if (media.base_url.empty()) return;
        std::string key = std::to_string(media.id) + ":" + std::to_string(media.codecid) + ":" + media.base_url;
        if (!unique.insert(key).second) return;
        std::string label = audioLabel(media, kind);
        const auto estimate = estimateMediaBytes(videoMedia, &media, base.duration_seconds);
        if (estimate > 0) label += " · ~" + humanDownloadBytes(estimate);
        choices.push_back({media, std::move(label)});
    };
    if (result.dash.has_flac) append(result.dash.flac_audio, "wiliwili/setting/app/playback/hi_res"_i18n);
    const std::string dolbyLabel = "wiliwili/setting/app/playback/dolby"_i18n;
    for (const auto& media : result.dash.dolby_audio) append(media, dolbyLabel);
    auto standard = result.dash.audio;
    std::stable_sort(standard.begin(), standard.end(), [](const auto& a, const auto& b) { return a.bandwidth > b.bandwidth; });
    for (const auto& media : standard) append(media);

    const auto qualityDesc = downloadQualityDesc(result, quality);
    auto enqueue = [base, videoMedia, quality, qualityDesc, onSelected](const bilibili::DashMedia* audioMedia,
                                                                        const std::string& selectedAudio) mutable {
        DownloadTask task = base;
        task.is_dash = true;
        task.quality = quality;
        task.quality_desc = qualityDesc;
        task.video_codec_id = videoMedia.codecid;
        task.video_bandwidth = videoMedia.bandwidth;
        task.video_width = videoMedia.width;
        task.video_height = videoMedia.height;
        task.video_urls = mediaUrls(videoMedia);
        task.audio_desc = selectedAudio;
        if (audioMedia) {
            task.audio_id = audioMedia->id;
            task.audio_codec_id = audioMedia->codecid;
            task.audio_bandwidth = audioMedia->bandwidth;
            task.audio_urls = mediaUrls(*audioMedia);
        }
        task.estimated_bytes = estimateMediaBytes(videoMedia, audioMedia, task.duration_seconds);
        onSelected(std::move(task));
    };

    if (choices.empty()) {
        enqueue(nullptr, "wiliwili/player/download/no_audio"_i18n);
        return true;
    }

    std::vector<std::string> audioLabels;
    for (const auto& choice : choices) audioLabels.push_back(choice.label);
    auto* audioDropdown = makeDownloadDropdown(
        "wiliwili/player/download/select_audio"_i18n, audioLabels,
        [enqueue, choices](int selected) mutable {
            if (selected < 0 || static_cast<size_t>(selected) >= choices.size()) return;
            const auto& choice = choices[selected];
            enqueue(&choice.media, choice.label);
        });
    brls::Application::pushActivity(new brls::Activity(audioDropdown));
    return true;
}

bool showDownloadCodecDropdown(DownloadTask base, const bilibili::VideoUrlResult& result, int quality,
                               DownloadSelectionCallback onSelected) {
    std::vector<bilibili::DashMedia> videos;
    std::set<int> codecs;
    for (const auto& media : result.dash.video) {
        if (media.id == quality && !media.base_url.empty()) {
            videos.push_back(media);
            codecs.insert(media.codecid);
        }
    }
    if (videos.empty()) return false;

    const auto pref = ProgramConfig::instance().getSettingItem(SettingItem::DOWNLOAD_VIDEO_CODEC, std::string{"auto"});
    auto rank = [&](const bilibili::DashMedia& media) {
        if (pref == "avc" && media.codecid == 7) return 0;
        if (pref == "hevc" && media.codecid == 12) return 0;
        if (pref == "av1" && media.codecid == 13) return 0;
        if (pref == "auto" && media.codecid == BILI::VIDEO_CODEC) return 0;
        return 1;
    };
    std::stable_sort(videos.begin(), videos.end(), [&](const auto& a, const auto& b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a.bandwidth > b.bandwidth;
    });

    // Keep one best representation per codec to avoid duplicate CDN variants in the selector.
    std::vector<bilibili::DashMedia> codecChoices;
    std::set<int> seen;
    for (const auto& media : videos) if (seen.insert(media.codecid).second) codecChoices.push_back(media);
    if (codecChoices.size() == 1) return showDownloadAudioForVideo(base, result, codecChoices.front(), quality, onSelected);

    std::vector<std::string> labels;
    for (const auto& media : codecChoices) {
        std::string label = videoCodecLabel(media.codecid);
        if (media.bandwidth > 0) label += fmt::format(" · {} kbps", media.bandwidth / 1000);
        const auto estimate = estimateMediaBytes(media, nullptr, base.duration_seconds);
        if (estimate > 0) label += " · video ~" + humanDownloadBytes(estimate);
        labels.push_back(std::move(label));
    }
    auto* dropdown = makeDownloadDropdown(
        "wiliwili/player/download/select_codec"_i18n, labels,
        [base, result, quality, codecChoices, onSelected](int selected) mutable {
            if (selected < 0 || static_cast<size_t>(selected) >= codecChoices.size()) return;
            showDownloadAudioForVideo(base, result, codecChoices[selected], quality, onSelected);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

}

void BasePlayerActivity::showDownloadDialog() {
    if (videoUrlResult.dash.video.empty() && videoUrlResult.durl.empty()) {
        brls::Application::notify("wiliwili/player/download/not_ready"_i18n);
        return;
    }

    DownloadTask base;
    if (dynamic_cast<PlayerSeasonActivity*>(this)) {
        base.is_pgc = true;
        base.bvid = episodeResult.bvid;
        base.cid = episodeResult.cid;
        base.aid = episodeResult.aid;
        base.series_title = seasonInfo.season_title;
        base.part_title = episodeResult.title;
        if (!episodeResult.long_title.empty()) base.part_title += " " + episodeResult.long_title;
        base.title = base.series_title + (base.part_title.empty() ? "" : " - " + base.part_title);
        base.part_index = static_cast<int>(episodeResult.index) + 1;
        base.duration_seconds = static_cast<int>(episodeResult.duration);
        base.owner_name = seasonInfo.up_info.uname;
        base.cover_url = seasonInfo.cover;
        base.source_page_url = episodeResult.link.empty()
            ? ("https://www.bilibili.com/bangumi/play/ep" + std::to_string(episodeResult.id))
            : episodeResult.link;
    } else {
        base.bvid = videoDetailResult.bvid;
        base.cid = videoDetailPage.cid;
        base.aid = videoDetailResult.aid;
        base.series_title = videoDetailResult.title;
        base.part_title = videoDetailPage.part.empty() ? videoDetailResult.title : videoDetailPage.part;
        base.title = videoDetailResult.title;
        if (videoDetailResult.pages.size() > 1 && !videoDetailPage.part.empty()) base.title += " - " + videoDetailPage.part;
        base.part_index = static_cast<int>(videoDetailPage.page);
        base.duration_seconds = videoDetailPage.duration;
        base.owner_name = videoDetailResult.owner.name;
        base.cover_url = videoDetailResult.pic;
        base.source_page_url = "https://www.bilibili.com/video/" + videoDetailResult.bvid;
        if (videoDetailPage.page > 1) base.source_page_url += "?p=" + std::to_string(videoDetailPage.page);
    }

    if ((!base.is_pgc && base.bvid.empty()) || base.cid == 0) {
        brls::Application::notify("wiliwili/player/download/not_ready"_i18n);
        return;
    }

    auto enqueueSelection = [this](DownloadTask selected) mutable {
        const auto pgcValidCount = std::count_if(episodeList.begin(), episodeList.end(),
                                                 [](const auto& ep) { return ep.cid != 0; });
        const auto ugcValidCount = std::count_if(videoDetailResult.ugc_season.episodes.begin(),
                                                 videoDetailResult.ugc_season.episodes.end(),
                                                 [](const auto& ep) { return ep.cid != 0 && !ep.bvid.empty(); });
        const bool canBatch = selected.is_pgc ? pgcValidCount > 1
                                               : (videoDetailResult.pages.size() > 1 || ugcValidCount > 1);
        auto enqueueCurrent = [selected]() mutable {
            DownloadManager::instance().addTask(std::move(selected));
            brls::Application::notify("wiliwili/player/download/queued"_i18n);
        };
        if (!canBatch) {
            enqueueCurrent();
            return;
        }

        auto* dialog = new brls::Dialog("wiliwili/player/download/select_scope"_i18n);
        dialog->addButton("wiliwili/player/download/current_only"_i18n, enqueueCurrent);
        dialog->addButton("wiliwili/player/download/all_items"_i18n, [this, selected]() mutable {
            std::set<std::pair<std::string, uint64_t>> existing;
            for (const auto& task : DownloadManager::instance().getTasksSnapshot()) {
                if (task.status != DownloadTaskStatus::FAILED && task.status != DownloadTaskStatus::CANCELLED)
                    existing.insert({task.bvid, task.cid});
            }

            auto resetTransport = [](DownloadTask& task) {
                task.id.clear(); task.dir.clear(); task.output_file.clear(); task.muxed = false;
                task.video_urls.clear(); task.audio_urls.clear(); task.flv_segments.clear();
                task.downloaded_bytes = task.total_bytes = 0;
                task.audio_downloaded_bytes = task.audio_total_bytes = 0;
                task.finished_at = 0; task.error_message.clear();
            };

            std::vector<DownloadTask> batch;
            int64_t estimatedTotal = 0;
            if (selected.is_pgc) {
                int index = 0;
                for (const auto& ep : episodeList) {
                    if (ep.cid == 0) continue;
                    ++index;
                    if (existing.count({ep.bvid, ep.cid}) != 0) continue;
                    DownloadTask task = selected;
                    resetTransport(task);
                    task.bvid = ep.bvid; task.cid = ep.cid; task.aid = ep.aid;
                    task.part_index = index;
                    task.duration_seconds = static_cast<int>(ep.duration);
                    task.part_title = ep.title;
                    if (!ep.long_title.empty()) task.part_title += " " + ep.long_title;
                    task.title = task.series_title + (task.part_title.empty() ? "" : " - " + task.part_title);
                    task.source_page_url = ep.link.empty()
                        ? ("https://www.bilibili.com/bangumi/play/ep" + std::to_string(ep.id)) : ep.link;
                    task.estimated_bytes = DownloadManager::estimateBytes(task);
                    estimatedTotal += task.estimated_bytes;
                    batch.emplace_back(std::move(task));
                }
            } else {
                const auto ugcValidCount = std::count_if(videoDetailResult.ugc_season.episodes.begin(),
                                                         videoDetailResult.ugc_season.episodes.end(),
                                                         [](const auto& ep) { return ep.cid != 0 && !ep.bvid.empty(); });
                if (ugcValidCount > 1) {
                    int index = 0;
                    for (const auto& ep : videoDetailResult.ugc_season.episodes) {
                        if (ep.cid == 0 || ep.bvid.empty()) continue;
                        ++index;
                        if (existing.count({ep.bvid, ep.cid}) != 0) continue;
                        DownloadTask task = selected;
                        resetTransport(task);
                        task.bvid = ep.bvid;
                        task.cid = ep.cid;
                        task.aid = ep.aid;
                        task.part_index = index;
                        task.duration_seconds = ep.page.duration;
                        task.series_title = videoDetailResult.ugc_season.title.empty()
                                                ? selected.series_title
                                                : videoDetailResult.ugc_season.title;
                        task.part_title = ep.title.empty() ? ep.page.part : ep.title;
                        if (task.part_title.empty()) task.part_title = fmt::format("EP{}", index);
                        task.title = task.series_title + " - " + task.part_title;
                        // UGC season episode records do not contain their own cover. Resolve each
                        // BVID independently in DownloadManager instead of reusing one collection/current-video cover.
                        task.cover_url.clear();
                        task.source_page_url = "https://www.bilibili.com/video/" + task.bvid;
                        task.estimated_bytes = DownloadManager::estimateBytes(task);
                        estimatedTotal += task.estimated_bytes;
                        batch.emplace_back(std::move(task));
                    }
                } else {
                    for (const auto& page : videoDetailResult.pages) {
                        if (page.cid == 0) continue;
                        if (existing.count({selected.bvid, page.cid}) != 0) continue;
                        DownloadTask task = selected;
                        resetTransport(task);
                        task.cid = page.cid;
                        task.part_index = static_cast<int>(page.page);
                        task.duration_seconds = page.duration;
                        task.part_title = page.part.empty() ? fmt::format("P{}", page.page) : page.part;
                        task.title = task.series_title + " - " + task.part_title;
                        task.source_page_url = "https://www.bilibili.com/video/" + task.bvid;
                        if (page.page > 1) task.source_page_url += "?p=" + std::to_string(page.page);
                        task.estimated_bytes = DownloadManager::estimateBytes(task);
                        estimatedTotal += task.estimated_bytes;
                        batch.emplace_back(std::move(task));
                    }
                }
            }
            if (batch.empty()) {
                brls::Application::notify("wiliwili/player/download/already_exists"_i18n);
                return;
            }
            const int64_t freeBytes = DownloadManager::availableBytes(DownloadManager::defaultDownloadDir());
            // DASH muxing temporarily needs the downloaded tracks and the final MP4 at the same
            // time. The manager performs authoritative per-task reservations; this is an early
            // batch-level warning so users do not enqueue a season that obviously cannot fit.
            const int64_t muxReserve = selected.is_dash && selected.audio_id != 0 ? estimatedTotal : 0;
            const int64_t estimatedRequired = estimatedTotal + muxReserve + 128LL * 1024LL * 1024LL;
            if (estimatedTotal > 0 && freeBytes >= 0 && freeBytes < estimatedRequired) {
                brls::Application::notify(fmt::format("{}: ~{} / {}: {}",
                    "wiliwili/player/download/not_enough_space"_i18n, humanDownloadBytes(estimatedRequired),
                    "wiliwili/player/download/free_space"_i18n, humanDownloadBytes(freeBytes)));
                return;
            }
            DownloadManager::instance().addTasks(std::move(batch));
            brls::Application::notify("wiliwili/player/download/batch_queued"_i18n);
        });
        dialog->addButton("hints/cancel"_i18n, []() {});
        dialog->open();
    };

    for (const auto& existing : DownloadManager::instance().getTasksSnapshot()) {
        if (existing.bvid == base.bvid && existing.cid == base.cid &&
            existing.status != DownloadTaskStatus::FAILED && existing.status != DownloadTaskStatus::CANCELLED) {
            // Still allow the dialog when siblings exist: the user may want "download all" and the
            // batch builder will skip already queued/completed items. Single-item videos can stop here.
            const auto pgcValidCount = std::count_if(episodeList.begin(), episodeList.end(),
                                                     [](const auto& ep) { return ep.cid != 0; });
            const auto ugcValidCount = std::count_if(videoDetailResult.ugc_season.episodes.begin(),
                                                     videoDetailResult.ugc_season.episodes.end(),
                                                     [](const auto& ep) { return ep.cid != 0 && !ep.bvid.empty(); });
            const bool hasSiblings = base.is_pgc ? pgcValidCount > 1
                                                 : (videoDetailResult.pages.size() > 1 || ugcValidCount > 1);
            if (!hasSiblings) {
                brls::Application::notify("wiliwili/player/download/already_exists"_i18n);
                return;
            }
            break;
        }
    }

    // Legacy FLV already contains audio; the same scope selector can still queue all pages and
    // each sibling will refresh its own durl when the worker starts.
    if (videoUrlResult.dash.video.empty()) {
        auto* dialog = new brls::Dialog("wiliwili/player/download/select_quality"_i18n);
        dialog->addButton("wiliwili/player/download/source_stream"_i18n, [this, base, enqueueSelection]() mutable {
            DownloadTask task = base;
            task.is_dash = false;
            task.quality = videoUrlResult.quality;
            task.quality_desc = downloadQualityDesc(videoUrlResult, videoUrlResult.quality);
            task.audio_desc = "wiliwili/player/download/muxed_audio"_i18n;
            for (const auto& d : videoUrlResult.durl) {
                FlvSegment seg;
                seg.url = d.url; seg.backup_urls = d.backup_url;
                seg.size = d.size > 0 ? static_cast<uint64_t>(d.size) : 0; seg.order = d.order;
                task.flv_segments.push_back(std::move(seg));
            }
            task.estimated_bytes = DownloadManager::estimateBytes(task);
            enqueueSelection(std::move(task));
        });
        dialog->addButton("hints/cancel"_i18n, []() {});
        dialog->open();
        return;
    }

    std::map<int, bilibili::DashMedia> preferred;
    for (const auto& media : videoUrlResult.dash.video) {
        auto it = preferred.find(media.id);
        if (it == preferred.end() || media.bandwidth > it->second.bandwidth) preferred[media.id] = media;
    }

    std::vector<int> qualities;
    std::set<int> seen;
    for (int q : videoUrlResult.accept_quality) if (seen.insert(q).second) qualities.push_back(q);
    for (const auto& [q, _] : preferred) if (seen.insert(q).second) qualities.push_back(q);
    if (qualities.empty()) {
        brls::Application::notify("wiliwili/player/download/not_ready"_i18n);
        return;
    }

    const bilibili::DashMedia* bestAudio = nullptr;
    for (const auto& audio : videoUrlResult.dash.audio) {
        if (!bestAudio || audio.bandwidth > bestAudio->bandwidth) bestAudio = &audio;
    }
    if (videoUrlResult.dash.has_flac && (!bestAudio || videoUrlResult.dash.flac_audio.bandwidth > bestAudio->bandwidth))
        bestAudio = &videoUrlResult.dash.flac_audio;

    std::vector<std::string> qualityLabels;
    for (int q : qualities) {
        std::string label = downloadQualityDesc(videoUrlResult, q);
        auto it = preferred.find(q);
        if (it != preferred.end()) {
            if (it->second.width > 0 && it->second.height > 0) label += fmt::format(" · {}x{}", it->second.width, it->second.height);
            const auto estimate = estimateMediaBytes(it->second, bestAudio, base.duration_seconds);
            if (estimate > 0) label += " · ~" + humanDownloadBytes(estimate);
        }
        qualityLabels.push_back(std::move(label));
    }

    auto* qualityDropdown = makeDownloadDropdown(
        "wiliwili/player/download/select_quality"_i18n, qualityLabels,
        [this, base, preferred, qualities, enqueueSelection](int selected) mutable {
            if (selected < 0 || static_cast<size_t>(selected) >= qualities.size()) return;
            const int quality = qualities[selected];
            if (preferred.count(quality) != 0) {
                if (!showDownloadCodecDropdown(base, videoUrlResult, quality, enqueueSelection))
                    brls::Application::notify("wiliwili/player/download/not_ready"_i18n);
                return;
            }

            ASYNC_RETAIN
            auto success = [ASYNC_TOKEN, base, quality, enqueueSelection](const bilibili::VideoUrlResult& result) mutable {
                brls::sync([ASYNC_TOKEN, base, quality, result, enqueueSelection]() mutable {
                    ASYNC_RELEASE
                    if (!showDownloadCodecDropdown(base, result, quality, enqueueSelection))
                        brls::Application::notify("wiliwili/player/download/not_ready"_i18n);
                });
            };
            auto failure = [ASYNC_TOKEN](BILI_ERR) {
                brls::sync([ASYNC_TOKEN]() {
                    ASYNC_RELEASE
                    brls::Application::notify("wiliwili/player/download/not_ready"_i18n);
                });
            };
            if (base.is_pgc) {
                BILI::get_season_url(base.cid, quality,
                    [success](const bilibili::SeasonUrlResult& result) mutable { success(result.video_info); }, failure);
            } else {
                BILI::get_video_url(base.bvid, base.cid, quality, success, failure);
            }
        });

    ASYNC_RETAIN
    brls::sync([ASYNC_TOKEN, qualityDropdown]() {
        ASYNC_RELEASE
        brls::Application::pushActivity(new brls::Activity(qualityDropdown));
    });
}

void BasePlayerActivity::showCollectionDialog(uint64_t id, int videoType) {
    if (!DialogHelper::checkLogin()) return;
    auto playerCollection = new PlayerCollection(id, videoType);
    auto dialog           = new brls::Dialog(playerCollection);
    dialog->addButton("wiliwili/home/common/save"_i18n, [this, id, videoType, playerCollection]() {
        this->addResource(id, videoType, playerCollection->isFavorite(), playerCollection->getAddCollectionList(),
                          playerCollection->getDeleteCollectionList());
    });
    playerCollection->registerAction(
        "", brls::ControllerButton::BUTTON_START,
        [this, id, videoType, playerCollection, dialog](...) {
            this->addResource(id, videoType, playerCollection->isFavorite(), playerCollection->getAddCollectionList(),
                              playerCollection->getDeleteCollectionList());
            dialog->dismiss();
            return true;
        },
        true);
    dialog->open();
}

void BasePlayerActivity::showCoinDialog(uint64_t aid) {
    if (!DialogHelper::checkLogin()) return;

    if (std::to_string(videoDetailResult.owner.mid) == ProgramConfig::instance().getUserID()) {
        DialogHelper::showDialog("wiliwili/player/coin/own"_i18n);
        return;
    }

    int coins = getCoinTolerate();
    if (coins <= 0) {
        DialogHelper::showDialog("wiliwili/player/coin/run_out"_i18n);
        return;
    }

    auto playerCoin = new PlayerCoin();
    if (coins == 1) playerCoin->hideTwoCoin();
    playerCoin->getSelectEvent()->subscribe(
        [this, playerCoin, aid](int value) { this->addCoin(aid, value, playerCoin->likeAtTheSameTime()); });
    auto dialog = new brls::Dialog(playerCoin);
    dialog->open();
}

void BasePlayerActivity::updateVideoLink() {
    // 设置视频加载后跳转的时间
    setProgress(MPVCore::instance().video_progress);

    // dash
    if (!this->videoUrlResult.dash.video.empty()) {
        // dash格式的视频无需重复请求视频链接，这里简单的设置清晰度即可
        videoUrlResult.quality = BasePlayerActivity::defaultQuality;
        this->onVideoPlayUrl(videoUrlResult);
        return;
    }

    // flv
    if (dynamic_cast<PlayerSeasonActivity*>(this)) {
        this->requestSeasonVideoUrl(episodeResult.bvid, episodeResult.cid);
    } else {
        this->requestVideoUrl(videoDetailResult.bvid, videoDetailPage.cid);
    }
}

void BasePlayerActivity::setVideoQuality() {
    if (this->videoUrlResult.accept_description.empty()) return;

    auto* dropdown = new BaseDropdown(
        "wiliwili/player/quality"_i18n,
        [this](int selected) {
            int code                           = this->videoUrlResult.accept_quality[selected];
#ifdef __PSV__
            if (code > 64) {
                code = 64;
            }
#endif
            BasePlayerActivity::defaultQuality = code;
            ProgramConfig::instance().setSettingItem(SettingItem::VIDEO_QUALITY, code);

            // 如果未登录选择了大于等于480P清晰度的视频
            if (ProgramConfig::instance().getCSRF().empty() && defaultQuality >= 32) {
                DialogHelper::showDialog("wiliwili/home/common/no_login"_i18n);
                return;
            }

            this->updateVideoLink();
        },
        getQualityIndex());
    auto* recycler = dropdown->getRecyclingList();
    recycler->registerCell("Cell", []() { return new QualityCell(); });
    dropdown->setDataSource(new QualityDataSource(this->videoUrlResult, dropdown));
    dropdown->registerAction(
        "", brls::ControllerButton::BUTTON_START,
        [dropdown](...) {
            dropdown->dismiss();
            return true;
        },
        true);
    dropdown->registerAction(ShortcutHelper::getVideoQuality(), [dropdown](...) {
        dropdown->dismiss();
        return true;
    });

    // 因为触摸的问题 视频组件上开启新的 activity 需要同步执行
    // 不然在某些情况下焦点会错乱
    ASYNC_RETAIN
    brls::sync([ASYNC_TOKEN, dropdown]() {
        ASYNC_RELEASE
        brls::Application::pushActivity(new brls::Activity(dropdown));
    });
}

void BasePlayerActivity::setCommentMode() {
    this->recyclingGrid->estimatedRowHeight = 100;
    this->recyclingGrid->showSkeleton();
    tabFrame->focusTab(0);
    requestVideoComment(std::to_string(this->getAid()), 0, getVideoCommentMode() == 3 ? 2 : 3);
}

void BasePlayerActivity::onVideoPlayUrl(const bilibili::VideoUrlResult& result) {
    brls::Logger::debug("onVideoPlayUrl quality: {}", result.quality);

    if (result.accept_quality.empty() || result.accept_description.empty()) {
        // 通常是返回了其他报错信息, 比如验证码
        brls::Logger::error("onVideoPlayUrl: no video url available");
        auto dialog = new brls::Dialog("Error: No video url available");
        dialog->setCancelable(false);
        dialog->addButton("hints/ok"_i18n, []() {
            brls::sync([]() { brls::Application::popActivity(); });
        });
        dialog->open();
        return;
    }

    // 有效期 110 分钟
    videoDeadline = std::chrono::system_clock::now() + std::chrono::seconds(6600);

    // 获取预设的跳转位置
    int start    = this->getProgress();
    int end      = -1;
    int clipOpen = result.clipOpen, clipEnd = result.clipEnd;
    int time_sec = result.timelength / 1000;
    std::string customAspect;

    // 加载覆盖设置
    if (seasonInfo.season_id > 0) {
        auto seasonSetting = ProgramConfig::instance().getSeasonCustom(seasonInfo.season_id);
        customAspect       = seasonSetting.player_aspect;

        if (seasonSetting.custom_clip) {
            // 设置了自定义的片头片尾
            // 判断是否是正片
            for (auto& e : seasonInfo.episodes) {
                if (episodeResult.id == e.id) {
                    // 如果当前播放的是正片，则使用自定义的片头片尾
                    clipOpen = seasonSetting.clip_start;
                    if (clipOpen > time_sec - 5) clipOpen = 0;
                    clipEnd = time_sec - seasonSetting.clip_end;
                    if (seasonSetting.clip_end <= 0 || clipEnd < clipOpen) clipEnd = 0;
                    break;
                }
            }
        }
    }
    if (customAspect.empty()) {
        customAspect = ProgramConfig::instance().getSettingItem(SettingItem::PLAYER_ASPECT, std::string{"-1"});
    }
    MPVCore::instance().setAspect(customAspect);

    // 当要跳转的进度距离尾部只有 5s，就重新播放
    if (start > 0 && abs(time_sec - start) <= 5) start = 0;

    // 跳过片头片尾
    if (PLAYER_SKIP_OPENING_CREDITS) {
        start = std::max(start, clipOpen);
        if (clipEnd > 0) start = std::min(start, clipEnd);
        end = clipEnd;
        brls::Logger::debug("片头片尾: {}/{}", clipOpen, clipEnd);
    }

    // 针对用户上传的视频，尝试加载上一次播放的进度
    if (videoDetailPage.cid && start < 0) {
        auto data = SubtitleCore::instance().getSubtitleList();
        if (data.last_play_cid == videoDetailPage.cid && data.last_play_time > 0) {
            APP_E->fire(VideoView::LAST_TIME, (void*)&(data.last_play_time));
        }
    } else {
        // 设置为 POSITION_DISCARD 后，不会加载网络历史记录，而是直接使用 setProgress 指定的位置
        // 番剧视频或指定了进度的用户视频
        int64_t position = 0;
        APP_E->fire(VideoView::LAST_TIME, (void*)&(position));
        if (start > 0) {
            std::string hint = fmt::format("已为您定位至: {}", wiliwili::sec2Time(start));
            APP_E->fire(VideoView::HINT, (void*)hint.c_str());
        }
    }

    if (!result.dash.video.empty()) {
        // dash
        brls::Logger::debug("Video type: dash");

        // 找到当前可用的清晰度
        for (const auto& i : result.dash.video) {
            int desiredQuality = result.quality;
            // 若设置了过高的清晰度, 自动切换到合适的清晰度, 默认为 128 (即无限制)
            if (i.height > i.width) {
                desiredQuality = std::min(desiredQuality, portraitQualityMax);
            } else {
                desiredQuality = std::min(desiredQuality, landscapeQualityMax);
            }

            if (desiredQuality >= i.id) {
                videoUrlResult.quality = i.id;
                break;
            }
        }

        // 找到当前清晰度下可用的视频
        std::vector<bilibili::DashMedia> codecs;
        for (const auto& i : result.dash.video) {
            if (i.id == videoUrlResult.quality) {
                codecs.emplace_back(i);
            }
        }

        // 匹配当前设定的视频编码
        bilibili::DashMedia v = codecs[0];  // 默认是 AVC/H.264
        for (const auto& i : codecs) {
            if (BILI::VIDEO_CODEC == i.codecid) {
                v = i;
                break;
            }
        }

        // 将主音频和备份音频链接合并，当作不同的音轨传给播放器，可以实现在播放失败时自动切换
        std::vector<std::string> audios;
        if (!result.dash.audio.empty()) {
            // 选择音轨，支持杜比/无损优先和多级回退
            auto pickDolby = [&]() -> std::optional<bilibili::DashMedia> {
                if (result.dash.dolby_audio.empty()) return std::nullopt;
                bilibili::DashMedia best = result.dash.dolby_audio[0];
                for (auto& dm : result.dash.dolby_audio) if (dm.bandwidth > best.bandwidth) best = dm;
                return best;
            };
            auto pickFlac = [&]() -> std::optional<bilibili::DashMedia> {
                if (!result.dash.has_flac) return std::nullopt;
                return result.dash.flac_audio;
            };
            auto pickStandard = [&](int qid) -> std::optional<bilibili::DashMedia> {
                for (auto& i : result.dash.audio) if (i.id == qid) return i;
                return std::nullopt;
            };
            auto pickFirstAvailableStandard = [&]() -> std::optional<bilibili::DashMedia> {
                int candidates[] = {30280, 30232, 30216};
                for (int q : candidates) {
                    auto m = pickStandard(q);
                    if (m) return m;
                }
                // fallback to first item if none matched
                if (!result.dash.audio.empty()) return result.dash.audio[0];
                return std::nullopt;
            };

            bilibili::DashMedia a = result.dash.audio[0];
            bool selected = false;
            if (BILI::AUDIO_QUALITY == 30250) {
                // Dolby → Lossless → High → Medium → Low
                if (auto m = pickDolby()) { a = *m; selected = true; brls::Logger::debug("Picked Dolby audio (type {}), bw {}", result.dash.dolby_type, a.bandwidth);} else
                if (auto m = pickFlac()) { a = *m; selected = true; brls::Logger::debug("Picked FLAC audio, bw {}", a.bandwidth);} else
                if (auto m = pickFirstAvailableStandard()) { a = *m; selected = true; }
            } else if (BILI::AUDIO_QUALITY == 30251) {
                // Lossless → Dolby → High → Medium → Low
                if (auto m = pickFlac()) { a = *m; selected = true; brls::Logger::debug("Picked FLAC audio, bw {}", a.bandwidth);} else
                if (auto m = pickDolby()) { a = *m; selected = true; brls::Logger::debug("Picked Dolby audio (type {}), bw {}", result.dash.dolby_type, a.bandwidth);} else
                if (auto m = pickFirstAvailableStandard()) { a = *m; selected = true; }
            } else {
                // Try user-selected standard, then fallback High→Medium→Low
                if (auto m = pickStandard(BILI::AUDIO_QUALITY)) { a = *m; selected = true; }
                if (!selected) {
                    if (auto m = pickFirstAvailableStandard()) { a = *m; selected = true; }
                }
            }
            // 生成音频列表
            audios.emplace_back(a.base_url);
            audios.insert(audios.end(), a.backup_url.begin(), a.backup_url.end());
            brls::Logger::debug("Dash quality: {}; video: {}; audio: {}", videoUrlResult.quality, v.codecid, a.id);
        }

        // 给播放器设置链接
        this->video->setUrl(v.base_url, start, end, audios);

        // 设置备份视频链接
        for (const auto& backup_url : v.backup_url) {
            this->video->setBackupUrl(backup_url, start, end, audios);
        }
    } else {
        // flv
        brls::Logger::debug("Video type: flv");
        if (result.durl.empty()) {
            brls::Logger::error("No media");
        } else if (result.durl.size() == 1) {
            this->video->setUrl(result.durl[0].url, start, end);
        } else {
            std::vector<EDLUrl> urls;
            urls.reserve(result.durl.size());
            for (auto& i : result.durl) {
                urls.emplace_back(i.url, i.length / 1000.0f);
            }
            this->video->setUrl(urls, start, end);
        }
    }

    // 设置mpv事件
    // 1.更新清晰度
    std::string quality = videoUrlResult.accept_description[getQualityIndex()];
    APP_E->fire(VideoView::SET_QUALITY, (void*)quality.c_str());
    // 2.绘制进度条标记点（例如：片头片尾）
    if (clipOpen > 0) {
        float data = clipOpen * 1.0f / time_sec;
        APP_E->fire(VideoView::CLIP_INFO, (void*)&data);
    }
    if (clipEnd > 0) {
        float data2 = clipEnd * 1.0f / time_sec;
        APP_E->fire(VideoView::CLIP_INFO, (void*)&data2);
    }
    // 3. 设置视频时长
    APP_E->fire(VideoView::REAL_DURATION, (void*)&time_sec);

    brls::Logger::debug("BasePlayerActivity::onVideoPlayUrl done");

    // 根据配置决定是否自动全屏
    if (ProgramConfig::instance().getBoolOption(SettingItem::PLAYER_AUTO_FULLSCREEN) &&
        !this->video->isFullscreen()) {
        this->video->setFullScreen(true);
    }
}

void BasePlayerActivity::onCommentInfo(const bilibili::VideoCommentResultWrapper& result) {
    auto* datasource = dynamic_cast<DataSourceCommentList*>(recyclingGrid->getDataSource());
    if (!datasource && result.requestIndex == 0) {
        // 第一页评论
        //整合置顶评论
        std::vector<bilibili::VideoCommentResult> comments(result.top_replies);
        comments.insert(comments.end(), result.replies.begin(), result.replies.end());
        // 为了加载骨架屏美观，设置为了100，在加载评论时手动修改回来
        // 这里限制的是评论的最大高度，实际评论高度还受评论组件的最大行数限制
        this->recyclingGrid->estimatedRowHeight = 600;
        this->recyclingGrid->setDataSource(new DataSourceCommentList(
            comments, this->getAid(), this->getVideoCommentMode(), [this]() { this->setCommentMode(); }));
        this->recyclingGrid->selectRowAt(comments.empty() ? 1 : 2, false);
        // 设置评论数量提示
        auto item = this->tabFrame->getTab("wiliwili/player/comment"_i18n);
        if (item) item->setSubtitle(wiliwili::num2w(result.cursor.all_count));
    } else if (datasource) {
        // 第N页评论
        if (!result.replies.empty()) {
            datasource->appendData(result.replies);
            recyclingGrid->notifyDataChanged();
        }
    } else {
        brls::Logger::error("onCommentInfo ds: {} index: {} end: {}", (bool)datasource, result.requestIndex,
                            result.cursor.is_end);
    }
}

void BasePlayerActivity::onRequestCommentError(const std::string& error) {
    brls::sync([this, error]() { this->recyclingGrid->setError(error); });
}

void BasePlayerActivity::onVideoOnlineCount(const bilibili::VideoOnlineTotal& result) {
    std::string count = result.total + "wiliwili/player/current"_i18n;
    this->videoPeopleLabel->setText(count);
    APP_E->fire(VideoView::SET_ONLINE_NUM, (void*)count.c_str());
}

void BasePlayerActivity::onVideoRelationInfo(const bilibili::VideoRelation& result) {
    brls::Logger::debug("onVideoRelationInfo: {} {} {}", result.like, result.coin, result.favorite);
    this->setRelationButton(result.like, result.coin, result.favorite);
}

void BasePlayerActivity::onHighlightProgress(const bilibili::VideoHighlightProgress& result) {
    brls::Logger::debug("highlight: {}/{}", result.step_sec, result.data.size());
    VideoHighlightData data{result.step_sec, result.data};
    APP_E->fire(VideoView::HIGHLIGHT_INFO, (void*)&data);
}

void BasePlayerActivity::setRelationButton(bool liked, bool coin, bool favorite) {
    if (liked) {
        btnAgree->setImageFromSVGRes("svg/bpx-svg-sprite-liked-active.svg");
    } else {
        btnAgree->setImageFromSVGRes("svg/bpx-svg-sprite-liked.svg");
    }
    if (coin) {
        btnCoin->setImageFromSVGRes("svg/bpx-svg-sprite-coin-active.svg");
    } else {
        btnCoin->setImageFromSVGRes("svg/bpx-svg-sprite-coin.svg");
    }
    if (favorite) {
        btnFavorite->setImageFromSVGRes("svg/bpx-svg-sprite-collection-active.svg");
    } else {
        btnFavorite->setImageFromSVGRes("svg/bpx-svg-sprite-collection.svg");
    }
}

void BasePlayerActivity::onError(const std::string& error) {
    if (!activityShown) return;
    MPVCore::instance().stop();
    MPVCore::instance().reset();
    bool forceClose = true;
    std::string msg = error;
    if (pystring::count(error, "87007") > 0 || pystring::count(error, "87008") > 0) {
        forceClose = false;
        msg        = "该视频为「充电」专属视频";
    } else if (pystring::count(error, "10403") > 0) {
        forceClose = false;
        msg        = "大会员专享限制";
    } else if (pystring::count(error, "404") > 0) {
        msg = "啥都木有";
    } else if (pystring::count(error, "62002") > 0) {
        msg = "稿件不可见";
    }
    auto dialog = new brls::Dialog(msg);
    dialog->setCancelable(false);
    dialog->addButton("hints/ok"_i18n, [forceClose]() {
        if (forceClose) brls::sync([]() { brls::Application::popActivity(); });
    });
    dialog->open();
}

void BasePlayerActivity::willDisappear(bool resetState) {
    activityShown = false;
    brls::Activity::willDisappear(resetState);
}

void BasePlayerActivity::willAppear(bool resetState) {
    activityShown = true;
    brls::Activity::willAppear(resetState);
}

BasePlayerActivity::~BasePlayerActivity() {
    brls::Logger::debug("del BasePlayerActivity");
    // 取消监控mpv
    MPV_E->unsubscribe(eventSubscribeID);
    APP_E->unsubscribe(customEventSubscribeID);
    // 停止视频播放
    this->video->stop();
}