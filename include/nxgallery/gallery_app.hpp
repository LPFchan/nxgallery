#pragma once

#include <nxgallery/album_index.hpp>
#include <nxgallery/gallery_controller.hpp>
#include <nxgallery/telegram_bot.hpp>

#include <pu/Plutonium>

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

    void on_input(std::uint64_t down, pu::ui::TouchPoint touch);
    void on_touch(pu::ui::TouchPoint touch);
    void open_chat_picker();
    void start_share(ShareRequest request);
    void poll_share_worker();
    void advance_automation();

    GalleryController controller_;
    std::unique_ptr<TelegramBot> bot_;
    std::string status_;
    pu::ui::Layout::Ref layout_;
    std::shared_ptr<GalleryElement> element_;
    std::thread share_worker_;
    std::mutex share_mutex_;
    std::optional<BotResult> share_result_;
    bool touch_active_{};
    std::uint32_t automation_frame_{};
    bool automation_send_started_{};
};

}  // namespace nxgallery
