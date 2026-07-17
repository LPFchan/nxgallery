#include <nxgallery/gallery_app.hpp>
#include <nxgallery/horizon_album.hpp>
#include <nxgallery/video_player.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nxgallery {
namespace {

constexpr std::int32_t kWidth = 1280;
constexpr std::int32_t kHeight = 720;
constexpr pu::ui::Color kBackground{238, 238, 238, 255};
constexpr pu::ui::Color kPanel{250, 250, 250, 255};
constexpr pu::ui::Color kInk{42, 42, 46, 255};
constexpr pu::ui::Color kMuted{112, 112, 118, 255};
constexpr pu::ui::Color kAccent{0, 190, 220, 255};
constexpr pu::ui::Color kDark{19, 20, 24, 255};
constexpr pu::ui::Color kWhite{255, 255, 255, 255};
constexpr pu::ui::Color kSuccess{48, 170, 88, 255};
constexpr pu::ui::Color kFailure{210, 66, 74, 255};
constexpr std::int32_t kPickerPanelX = 190;
constexpr std::int32_t kPickerPanelY = 64;
constexpr std::int32_t kPickerPanelWidth = 900;
constexpr std::int32_t kPickerPanelHeight = 592;
constexpr std::int32_t kPickerRowX = 220;
constexpr std::int32_t kPickerRowY = 172;
constexpr std::int32_t kPickerRowWidth = 840;
constexpr std::int32_t kPickerRowHeight = 46;
constexpr std::int32_t kPickerRowStride = 52;
constexpr std::size_t kPickerVisibleRows = 7;
constexpr std::int32_t kPickerCancelX = 240;
constexpr std::int32_t kPickerSendX = 820;
constexpr std::int32_t kPickerButtonY = 548;
constexpr std::int32_t kPickerButtonWidth = 220;
constexpr std::int32_t kPickerButtonHeight = 54;

void render_outline(pu::ui::render::Renderer::Ref &drawer, pu::ui::Color color,
                    std::int32_t x, std::int32_t y, std::int32_t width,
                    std::int32_t height, std::int32_t stroke) {
    drawer->RenderRectangleFill(color, x, y, width, stroke);
    drawer->RenderRectangleFill(color, x, y + height - stroke, width, stroke);
    drawer->RenderRectangleFill(color, x, y, stroke, height);
    drawer->RenderRectangleFill(color, x + width - stroke, y, stroke, height);
}

std::string clipped(std::string value, std::size_t maximum) {
    if (value.size() <= maximum) return value;
    std::size_t prefix = maximum > 3 ? maximum - 3 : 0;
    while (prefix > 0 && prefix < value.size() &&
           (static_cast<unsigned char>(value[prefix]) & 0xC0U) == 0x80U) {
        --prefix;
    }
    value.resize(prefix);
    return value + "...";
}

}  // namespace

class GalleryApplication::GalleryElement final : public pu::ui::elm::Element {
public:
    GalleryElement(GalleryController &controller, std::string &status,
                   VideoPlayer &video_player,
                   std::atomic<std::uint64_t> &transfer_current,
                   std::atomic<std::uint64_t> &transfer_total)
        : controller_(controller), status_(status), video_player_(video_player),
          transfer_current_(transfer_current), transfer_total_(transfer_total) {}

    ~GalleryElement() override { clear_textures(); }

    s32 GetX() override { return 0; }
    s32 GetY() override { return 0; }
    s32 GetWidth() override { return kWidth; }
    s32 GetHeight() override { return kHeight; }
    void OnInput(const u64, const u64, const u64, const pu::ui::TouchPoint) override {}

    void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32, const s32) override {
        text_slot_ = 0;
        switch (controller_.screen()) {
            case Screen::Grid: render_grid(drawer); break;
            case Screen::Viewer: render_viewer(drawer); break;
            case Screen::ChatPicker: render_viewer(drawer); render_chat_picker(drawer); break;
            case Screen::Sending: render_viewer(drawer); render_sending(drawer); break;
            case Screen::Result: render_viewer(drawer); render_result(drawer); break;
        }
        for (std::size_t index = text_slot_; index < text_slots_.size(); ++index) {
            pu::ui::render::DeleteTexture(text_slots_[index].texture);
        }
        text_slots_.resize(text_slot_);
    }

private:
    struct TextSlot { std::string key; pu::sdl2::Texture texture{nullptr}; };
    struct ImageSlot { std::string path; pu::sdl2::Texture texture{nullptr}; };

    void clear_textures() {
        for (auto &slot : text_slots_) pu::ui::render::DeleteTexture(slot.texture);
        for (auto &slot : image_slots_) pu::ui::render::DeleteTexture(slot.texture);
        text_slots_.clear();
        image_slots_.clear();
    }

    void text(pu::ui::render::Renderer::Ref &drawer, const std::string &value,
              std::int32_t size, pu::ui::Color color, std::int32_t x,
              std::int32_t y) {
        const std::string key = std::to_string(size) + ":" +
            std::to_string(color.r) + ":" + std::to_string(color.g) + ":" +
            std::to_string(color.b) + ":" + std::to_string(color.a) + ":" + value;
        if (text_slot_ == text_slots_.size()) text_slots_.push_back({});
        TextSlot &slot = text_slots_[text_slot_++];
        if (slot.key != key) {
            pu::ui::render::DeleteTexture(slot.texture);
            slot.key = key;
            slot.texture = pu::ui::render::RenderText(
                pu::ui::MakeDefaultFontName(static_cast<std::uint32_t>(size)),
                value, color, 0, 0);
        }
        if (slot.texture != nullptr) drawer->RenderTexture(slot.texture, x, y);
    }

    pu::sdl2::Texture image(const std::string &path) {
        MediaItem media{path, path, MediaKind::Photo, 0, 0};
        std::string resolved_path;
        std::string error;
        if (!materialize_media_path(media, resolved_path, error)) {
            status_ = std::move(error);
            return nullptr;
        }
        auto found = std::find_if(image_slots_.begin(), image_slots_.end(),
                                  [&resolved_path](const ImageSlot &slot) { return slot.path == resolved_path; });
        if (found != image_slots_.end()) return found->texture;
        if (image_slots_.size() >= 16) {
            pu::ui::render::DeleteTexture(image_slots_.front().texture);
            image_slots_.erase(image_slots_.begin());
        }
        image_slots_.push_back({resolved_path, pu::ui::render::LoadImageFromFile(resolved_path)});
        return image_slots_.back().texture;
    }

    pu::sdl2::Texture thumbnail(const MediaItem &media) {
        std::string resolved_path;
        std::string error;
        if (!materialize_thumbnail_path(media, resolved_path, error)) {
            status_ = std::move(error);
            return nullptr;
        }
        auto found = std::find_if(image_slots_.begin(), image_slots_.end(),
                                  [&resolved_path](const ImageSlot &slot) {
                                      return slot.path == resolved_path;
                                  });
        if (found != image_slots_.end()) return found->texture;
        if (image_slots_.size() >= 16) {
            pu::ui::render::DeleteTexture(image_slots_.front().texture);
            image_slots_.erase(image_slots_.begin());
        }
        image_slots_.push_back(
            {resolved_path, pu::ui::render::LoadImageFromFile(resolved_path)});
        return image_slots_.back().texture;
    }

    void fitted_image(pu::ui::render::Renderer::Ref &drawer, pu::sdl2::Texture texture,
                      std::int32_t x, std::int32_t y, std::int32_t width,
                      std::int32_t height) {
        if (texture == nullptr) return;
        const std::int32_t source_width = pu::ui::render::GetTextureWidth(texture);
        const std::int32_t source_height = pu::ui::render::GetTextureHeight(texture);
        if (source_width <= 0 || source_height <= 0) return;
        const double scale = std::min(static_cast<double>(width) / source_width,
                                      static_cast<double>(height) / source_height);
        const std::int32_t render_width = static_cast<std::int32_t>(source_width * scale);
        const std::int32_t render_height = static_cast<std::int32_t>(source_height * scale);
        drawer->RenderTexture(texture, x + (width - render_width) / 2,
                              y + (height - render_height) / 2,
                              pu::ui::render::TextureRenderOptions({}, render_width,
                                  render_height, {}, {}, {}));
    }

    void header(pu::ui::render::Renderer::Ref &drawer, const std::string &title) {
        drawer->RenderRectangleFill(kPanel, 0, 0, kWidth, 72);
        text(drawer, title, 30, kInk, 42, 18);
        if (!status_.empty()) text(drawer, clipped(status_, 78), 18, kMuted, 420, 25);
    }

    void footer(pu::ui::render::Renderer::Ref &drawer, const std::string &hints,
                pu::ui::Color background = kPanel, pu::ui::Color ink = kInk) {
        drawer->RenderRectangleFill(background, 0, 665, kWidth, 55);
        text(drawer, hints, 22, ink, 48, 680);
    }

    void render_grid(pu::ui::render::Renderer::Ref &drawer) {
        drawer->RenderRectangleFill(kBackground, 0, 0, kWidth, kHeight);
        header(drawer, "Album");
        const auto &media = controller_.media();
        if (media.empty()) {
            text(drawer, "No captures found", 34, kInk, 470, 300);
            text(drawer, "Expected SD path: /Nintendo/Album", 20, kMuted, 450, 350);
        }
        const std::size_t start = controller_.grid_page_start();
        constexpr std::int32_t cell_width = 276;
        constexpr std::int32_t cell_height = 160;
        for (std::size_t visible = 0; visible < GalleryController::kGridColumns * GalleryController::kVisibleRows; ++visible) {
            const std::size_t index = start + visible;
            if (index >= media.size()) break;
            const std::int32_t column = static_cast<std::int32_t>(visible % GalleryController::kGridColumns);
            const std::int32_t row = static_cast<std::int32_t>(visible / GalleryController::kGridColumns);
            const std::int32_t x = 44 + column * 303;
            const std::int32_t y = 88 + row * 184;
            drawer->RenderRectangleFill(kDark, x, y, cell_width, cell_height);
            fitted_image(drawer, thumbnail(media[index]), x, y, cell_width, cell_height);
            if (index == controller_.selected_media_index()) render_outline(drawer, kAccent, x - 5, y - 5, cell_width + 10, cell_height + 10, 5);
        }
        footer(drawer, "A  View                                      +  hbmenu");
    }

    void render_viewer(pu::ui::render::Renderer::Ref &drawer) {
        drawer->RenderRectangleFill(kDark, 0, 0, kWidth, kHeight);
        drawer->RenderRoundedRectangle({42, 44, 50, 255}, 28, 22, 132, 54, 12);
        text(drawer, "Back", 22, kWhite, 66, 36);
        drawer->RenderRoundedRectangle(kAccent, 1050, 22, 190, 54, 12);
        text(drawer, "Share", 22, kDark, 1113, 36);
        const auto &media = controller_.media();
        if (!media.empty()) {
            const MediaItem &selected = media[controller_.selected_media_index()];
            if (selected.kind == MediaKind::Photo) fitted_image(drawer, image(selected.path), 80, 90, 1120, 520);
            else {
                pu::sdl2::Texture frame = video_player_.texture();
                fitted_image(drawer, frame != nullptr ? frame : thumbnail(selected),
                             80, 90, 1120, 520);
            }
            text(drawer, clipped(selected.filename, 58), 18, kWhite, 34, 634);
        }
        const bool video_selected = !media.empty() &&
            media[controller_.selected_media_index()].kind == MediaKind::Video;
        footer(drawer, video_selected
            ? "A  Play/Pause    B  Back    X  Share    ◀/▶  Previous/Next    +  hbmenu"
            : "B  Back          X  Share          ◀/▶  Previous/Next          +  hbmenu",
            kDark, kWhite);
    }

    void render_chat_picker(pu::ui::render::Renderer::Ref &drawer) {
        drawer->RenderRectangleFill({0, 0, 0, 150}, 0, 0, kWidth, kHeight);
        drawer->RenderRectangleFill(kPanel, kPickerPanelX, kPickerPanelY,
                                    kPickerPanelWidth, kPickerPanelHeight);
        text(drawer, "Share to Telegram", 30, kInk, 232, 94);
        text(drawer, clipped(status_, 72), 17, kMuted, 232, 136);
        const auto &chats = controller_.chats();
        if (chats.empty()) {
            text(drawer, "No chats discovered yet.", 25, kInk, 420, 300);
            text(drawer, "Message the bot or add a chat=ID|Title entry.", 18, kMuted, 375, 350);
        }
        const std::size_t selected = controller_.selected_chat_index();
        const std::size_t start = selected >= 6 ? selected - 5 : 0;
        for (std::size_t row = 0; row < kPickerVisibleRows && start + row < chats.size(); ++row) {
            const std::size_t index = start + row;
            const std::int32_t row_y = kPickerRowY +
                static_cast<std::int32_t>(row) * kPickerRowStride;
            if (index == selected) {
                drawer->RenderRectangleFill({218, 246, 250, 255}, kPickerRowX,
                                            row_y, kPickerRowWidth, kPickerRowHeight);
            }
            text(drawer, clipped(chats[index].title, 46), 23, kInk, 250, row_y + 7);
            text(drawer, chats[index].type, 16, kMuted, 890, row_y + 12);
        }
        drawer->RenderRoundedRectangle({224, 224, 228, 255}, kPickerCancelX,
                                       kPickerButtonY, kPickerButtonWidth,
                                       kPickerButtonHeight, 10);
        text(drawer, "Cancel", 20, kInk, 318, kPickerButtonY + 16);
        drawer->RenderRoundedRectangle(kAccent, kPickerSendX, kPickerButtonY,
                                       kPickerButtonWidth, kPickerButtonHeight, 10);
        text(drawer, "Send", 20, kDark, 906, kPickerButtonY + 16);
        text(drawer, "A  Send          Y  Refresh          B  Cancel", 18, kMuted, 363, 620);
    }

    void render_sending(pu::ui::render::Renderer::Ref &drawer) {
        drawer->RenderRectangleFill({0, 0, 0, 170}, 0, 0, kWidth, kHeight);
        drawer->RenderRectangleFill(kPanel, 390, 230, 500, 250);
        text(drawer, "Sending to Telegram...", 29, kInk, 470, 278);
        const std::uint64_t current = transfer_current_.load();
        const std::uint64_t total = transfer_total_.load();
        const std::uint64_t percent = total > 0
            ? std::min<std::uint64_t>(100, current * 100 / total) : 0;
        drawer->RenderRoundedRectangle({220, 220, 224, 255}, 440, 350, 400, 24, 12);
        if (percent > 0) {
            drawer->RenderRoundedRectangle(kAccent, 440, 350,
                                           static_cast<std::int32_t>(percent * 4),
                                           24, 12);
        }
        text(drawer, total > 0 ? std::to_string(percent) + "%" : "Preparing upload...",
             19, kMuted, total > 0 ? 615 : 555, 390);
        text(drawer, "Keep NX Gallery open", 18, kMuted, 540, 433);
    }

    void render_result(pu::ui::render::Renderer::Ref &drawer) {
        drawer->RenderRectangleFill({0, 0, 0, 170}, 0, 0, kWidth, kHeight);
        drawer->RenderRectangleFill(kPanel, 320, 220, 640, 260);
        const bool success = controller_.share_succeeded();
        text(drawer, success ? "Shared" : "Could not share", 32,
             success ? kSuccess : kFailure, success ? 570 : 505, 270);
        text(drawer, clipped(controller_.result_message(), 62), 20, kInk, 390, 345);
        text(drawer, "A/B  Close", 19, kMuted, 575, 430);
    }

    GalleryController &controller_;
    std::string &status_;
    VideoPlayer &video_player_;
    std::atomic<std::uint64_t> &transfer_current_;
    std::atomic<std::uint64_t> &transfer_total_;
    std::vector<TextSlot> text_slots_;
    std::vector<ImageSlot> image_slots_;
    std::size_t text_slot_{};
};

GalleryApplication::GalleryApplication(pu::ui::render::Renderer::Ref renderer,
                                       AlbumScanResult album,
                                       std::unique_ptr<TelegramBot> bot,
                                       std::string telegram_status)
    : Application(std::move(renderer)), bot_(std::move(bot)),
      status_(std::move(telegram_status)) {
    controller_.set_media(std::move(album.items));
    if (!album.error.empty()) status_ = album.error;
    else if (album.truncated) status_ = "Album limited to the newest 5000 captures";
}

GalleryApplication::~GalleryApplication() {
    if (share_worker_.joinable()) share_worker_.join();
    if (chat_refresh_worker_.joinable()) chat_refresh_worker_.join();
}

void GalleryApplication::OnLoad() {
    video_player_ = std::make_unique<VideoPlayer>(pu::ui::render::GetMainRenderer());
    layout_ = pu::ui::Layout::New();
    layout_->SetBackgroundColor(kBackground);
    element_ = std::make_shared<GalleryElement>(controller_, status_, *video_player_,
                                                transfer_current_, transfer_total_);
    layout_->Add(element_);
    LoadLayout(layout_);
    SetOnInput([this](const u64 down, const u64, const u64, const pu::ui::TouchPoint touch) {
        on_input(down, touch);
    });
    AddRenderCallback([this] {
        if (video_player_) {
            video_player_->update_texture();
            if (video_player_->active() || video_player_->status() == "Playback finished") {
                status_ = video_player_->status();
            }
        }
        poll_share_worker();
        poll_chat_refresh();
        advance_automation();
    });
    if (bot_) {
        std::vector<TelegramChat> cached;
        bot_->cached_chats(cached);
        controller_.set_chats(std::move(cached));
        start_chat_refresh();
    }
}

void GalleryApplication::advance_automation() {
#ifdef NXGALLERY_AUTOMATION_BUILD
    constexpr char kSendTrigger[] = "sdmc:/switch/nxgallery/automation-send";
    ++automation_frame_;
    if (automation_frame_ == 120 && controller_.screen() == Screen::Grid) {
        controller_.handle(Action::Confirm);
    } else if (automation_frame_ == 180 && controller_.screen() == Screen::Viewer &&
               !controller_.media().empty() &&
               controller_.media()[controller_.selected_media_index()].kind == MediaKind::Video) {
        video_player_->play(controller_.media()[controller_.selected_media_index()]);
    } else if (automation_frame_ == 240 && video_player_->active()) {
        video_player_->toggle_pause();
    } else if (automation_frame_ == 250 && video_player_->paused()) {
        automation_paused_frames_ = video_player_->frames_decoded();
    } else if (automation_frame_ == 300 && video_player_->paused()) {
        const bool frozen = video_player_->frames_decoded() == automation_paused_frames_;
        std::printf("NXGALLERY_AUTOMATION phase=video_pause result=%s frames=%llu\n",
                    frozen ? "pass" : "fail",
                    static_cast<unsigned long long>(video_player_->frames_decoded()));
        video_player_->toggle_pause();
    } else if (automation_frame_ == 420 && controller_.screen() == Screen::Viewer) {
        open_chat_picker();
    } else if (automation_frame_ > 420 && !automation_send_started_ &&
               controller_.screen() == Screen::ChatPicker) {
        std::FILE *trigger = std::fopen(kSendTrigger, "rb");
        if (trigger == nullptr) return;
        std::fclose(trigger);
        if (std::remove(kSendTrigger) != 0) {
            status_ = "Could not consume automation send trigger";
            return;
        }
        automation_send_started_ = true;
        auto request = controller_.handle(Action::Confirm);
        if (request) start_share(std::move(*request));
    }
#endif
}

void GalleryApplication::open_chat_picker() {
    if (video_player_) video_player_->stop();
    if (!bot_) status_ = "Telegram is not configured";
    controller_.handle(Action::Share);
}

void GalleryApplication::start_share(ShareRequest request) {
    if (!bot_ || share_worker_.joinable()) {
        controller_.finish_share(false, "Telegram is unavailable");
        return;
    }
    transfer_current_ = 0;
    transfer_total_ = 0;
    share_worker_ = std::thread([this, request = std::move(request)]() mutable {
        BotResult result = bot_->send_media(
            request.media, request.chat,
            [this](std::uint64_t current, std::uint64_t total) {
                transfer_current_ = current;
                transfer_total_ = total;
            });
        std::lock_guard<std::mutex> lock(share_mutex_);
        share_result_ = std::move(result);
    });
}

void GalleryApplication::start_chat_refresh() {
    if (!bot_ || chat_refresh_worker_.joinable()) return;
    chat_refresh_worker_ = std::thread([this] {
        std::vector<TelegramChat> chats;
        BotResult result = bot_->refresh_chats(chats);
        std::lock_guard<std::mutex> lock(chat_refresh_mutex_);
        refreshed_chats_ = std::move(chats);
        chat_refresh_result_ = std::move(result);
    });
}

void GalleryApplication::poll_chat_refresh() {
    std::optional<BotResult> result;
    std::vector<TelegramChat> chats;
    {
        std::lock_guard<std::mutex> lock(chat_refresh_mutex_);
        if (!chat_refresh_result_) return;
        result.swap(chat_refresh_result_);
        chats.swap(refreshed_chats_);
    }
    if (chat_refresh_worker_.joinable()) chat_refresh_worker_.join();
    controller_.set_chats(std::move(chats));
    status_ = std::move(result->message);
}

void GalleryApplication::poll_share_worker() {
    std::optional<BotResult> result;
    {
        std::lock_guard<std::mutex> lock(share_mutex_);
        result.swap(share_result_);
    }
    if (!result) return;
    if (share_worker_.joinable()) share_worker_.join();
    controller_.finish_share(result->success, std::move(result->message));
}

void GalleryApplication::on_touch(pu::ui::TouchPoint touch) {
    if (controller_.screen() == Screen::Grid) {
        const std::size_t start = controller_.grid_page_start();
        for (std::size_t visible = 0; visible < GalleryController::kGridColumns * GalleryController::kVisibleRows; ++visible) {
            const std::size_t index = start + visible;
            if (index >= controller_.media().size()) break;
            const std::int32_t column = static_cast<std::int32_t>(visible % GalleryController::kGridColumns);
            const std::int32_t row = static_cast<std::int32_t>(visible / GalleryController::kGridColumns);
            if (touch.HitsRegion(44 + column * 303, 88 + row * 184, 276, 160)) {
                controller_.select_media(index);
                controller_.handle(Action::Confirm);
                return;
            }
        }
        return;
    }
    if (controller_.screen() == Screen::Viewer) {
        if (touch.HitsRegion(28, 22, 132, 54)) controller_.handle(Action::Back);
        else if (touch.HitsRegion(1050, 22, 190, 54)) open_chat_picker();
        return;
    }
    if (controller_.screen() == Screen::ChatPicker) {
        if (touch.HitsRegion(kPickerCancelX, kPickerButtonY,
                             kPickerButtonWidth, kPickerButtonHeight)) {
            controller_.handle(Action::Back);
            return;
        }
        if (touch.HitsRegion(kPickerSendX, kPickerButtonY,
                             kPickerButtonWidth, kPickerButtonHeight)) {
            auto request = controller_.handle(Action::Confirm);
            if (request) start_share(std::move(*request));
            return;
        }
        const std::size_t selected = controller_.selected_chat_index();
        const std::size_t start = selected >= 6 ? selected - 5 : 0;
        for (std::size_t row = 0; row < kPickerVisibleRows &&
             start + row < controller_.chats().size(); ++row) {
            if (touch.HitsRegion(kPickerRowX, kPickerRowY +
                                 static_cast<std::int32_t>(row) * kPickerRowStride,
                                 kPickerRowWidth, kPickerRowHeight)) {
                controller_.select_chat(start + row);
                return;
            }
        }
        return;
    }
    if (controller_.screen() == Screen::Result) controller_.handle(Action::Confirm);
}

void GalleryApplication::on_input(std::uint64_t down, pu::ui::TouchPoint touch) {
    if (touch.IsEmpty()) touch_active_ = false;
    else if (!touch_active_) {
        touch_active_ = true;
        on_touch(touch);
    }
    if ((down & HidNpadButton_Plus) != 0 && controller_.screen() != Screen::Sending) {
        const bool supported = envHasNextLoad();
        const Result result = supported
            ? envSetNextLoad("sdmc:/hbmenu.nro", "sdmc:/hbmenu.nro") : 0;
        std::printf("NXGALLERY_DIAGNOSTIC event=exit target=hbmenu supported=%s rc=0x%08x\n",
                    supported ? "true" : "false", static_cast<unsigned int>(result));
        std::fflush(stdout);
        if (video_player_) video_player_->stop();
        Close();
        return;
    }
    if (controller_.screen() == Screen::Sending) return;
    if (controller_.screen() == Screen::Viewer &&
        !controller_.media().empty() &&
        controller_.media()[controller_.selected_media_index()].kind == MediaKind::Video) {
        if ((down & HidNpadButton_A) != 0) {
            if (video_player_->active()) video_player_->toggle_pause();
            else video_player_->play(controller_.media()[controller_.selected_media_index()]);
            return;
        }
        if ((down & (HidNpadButton_B | HidNpadButton_Left | HidNpadButton_Right)) != 0) {
            video_player_->stop();
        }
    }
    if ((down & HidNpadButton_X) != 0 && controller_.screen() == Screen::Viewer) { open_chat_picker(); return; }
    if ((down & HidNpadButton_Y) != 0 && controller_.screen() == Screen::ChatPicker) {
        if (!bot_) status_ = "Telegram is not configured";
        else if (chat_refresh_worker_.joinable()) status_ = "Chat refresh already running";
        else {
            status_ = "Refreshing chats in background...";
            start_chat_refresh();
        }
        return;
    }
    std::optional<ShareRequest> request;
    if ((down & HidNpadButton_A) != 0) request = controller_.handle(Action::Confirm);
    else if ((down & HidNpadButton_B) != 0) controller_.handle(Action::Back);
    else if ((down & HidNpadButton_Left) != 0) controller_.handle(Action::Left);
    else if ((down & HidNpadButton_Right) != 0) controller_.handle(Action::Right);
    else if ((down & HidNpadButton_Up) != 0) controller_.handle(Action::Up);
    else if ((down & HidNpadButton_Down) != 0) controller_.handle(Action::Down);
    if (request) start_share(std::move(*request));
}

}  // namespace nxgallery
