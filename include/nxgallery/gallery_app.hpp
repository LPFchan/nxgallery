#pragma once

#include <nxgallery/album_index.hpp>
#include <nxgallery/gallery_controller.hpp>
#include <nxgallery/telegram_bot.hpp>
#include <nxgallery/token_setup.hpp>
#include <nxgallery/video_player.hpp>

#include <pu/Plutonium>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace nxgallery {

class GalleryApplication final : public pu::ui::Application {
public:
    GalleryApplication(pu::ui::render::Renderer::Ref renderer,
                       AlbumScanResult album,
                       std::unique_ptr<TelegramBot> bot,
                       std::string telegram_status);
    ~GalleryApplication() override;

    void OnLoad() override;

private:
    class GalleryElement;

    void on_input(std::uint64_t down, std::uint64_t held, pu::ui::TouchPoint touch);
    void on_touch(pu::ui::TouchPoint touch);
    void on_swipe(std::int32_t dx, std::int32_t dy);
    bool dispatch_hint(pu::ui::TouchPoint touch);
    void toggle_video_playback();
    void open_chat_picker();
    void refresh_chats_from_ui();
    void exit_to_hbmenu();
    void start_share(ShareRequest request);
    void poll_share_worker();
    void start_chat_refresh();
    void poll_chat_refresh();
    void open_token_setup();
    void close_token_setup();
    void poll_token_setup();
    void apply_setup_token();
    void advance_automation();

    GalleryController controller_;
    std::unique_ptr<TelegramBot> bot_;
    std::string status_;
    pu::ui::Layout::Ref layout_;
    std::shared_ptr<GalleryElement> element_;
    std::unique_ptr<VideoPlayer> video_player_;
    std::thread share_worker_;
    std::thread chat_refresh_worker_;
    std::mutex share_mutex_;
    std::mutex chat_refresh_mutex_;
    std::optional<BotResult> share_result_;
    std::optional<BotResult> chat_refresh_result_;
    std::vector<TelegramChat> refreshed_chats_;
    std::atomic<std::uint64_t> transfer_current_{};
    std::atomic<std::uint64_t> transfer_total_{};
    std::atomic<bool> transfer_cancel_requested_{};
    std::unique_ptr<TokenSetupServer> setup_server_;
    std::string setup_url_;
    std::string setup_notice_;
    std::string pending_setup_token_;
    bool setup_active_{};
    bool telegram_ready_{};
    std::array<std::uint32_t, 4> dir_hold_frames_{};
    bool touch_down_{};
    std::int32_t touch_start_x_{};
    std::int32_t touch_start_y_{};
    std::int32_t touch_last_x_{};
    std::int32_t touch_last_y_{};
    std::uint32_t automation_frame_{};
    bool automation_send_started_{};
    std::uint64_t automation_paused_frames_{};
};

}  // namespace nxgallery
