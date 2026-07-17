#include <nxgallery/gallery_app.hpp>

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
    GalleryElement(GalleryController &controller, std::string &status)
        : controller_(controller), status_(status) {}

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
        auto found = std::find_if(image_slots_.begin(), image_slots_.end(),
                                  [&path](const ImageSlot &slot) { return slot.path == path; });
        if (found != image_slots_.end()) return found->texture;
        if (image_slots_.size() >= 16) {
            pu::ui::render::DeleteTexture(image_slots_.front().texture);
            image_slots_.erase(image_slots_.begin());
        }
        image_slots_.push_back({path, pu::ui::render::LoadImageFromFile(path)});
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
            if (media[index].kind == MediaKind::Photo) fitted_image(drawer, image(media[index].path), x, y, cell_width, cell_height);
            else text(drawer, "VIDEO", 24, kWhite, x + 92, y + 66);
            if (index == controller_.selected_media_index()) render_outline(drawer, kAccent, x - 5, y - 5, cell_width + 10, cell_height + 10, 5);
        }
        footer(drawer, "A  View                                      +  Exit");
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
                text(drawer, "VIDEO", 48, kWhite, 550, 270);
                text(drawer, "Playback is not implemented in this slice", 22, kMuted, 425, 335);
            }
            text(drawer, clipped(selected.filename, 58), 18, kWhite, 34, 634);
        }
        footer(drawer, "B  Back          X  Share          ◀/▶  Previous/Next          +  Exit", kDark, kWhite);
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
        drawer->RenderRectangleFill(kPanel, 390, 250, 500, 190);
        text(drawer, "Sending to Telegram...", 29, kInk, 470, 315);
        text(drawer, "Keep NX Gallery open", 19, kMuted, 535, 365);
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
}

void GalleryApplication::OnLoad() {
    layout_ = pu::ui::Layout::New();
    layout_->SetBackgroundColor(kBackground);
    element_ = std::make_shared<GalleryElement>(controller_, status_);
    layout_->Add(element_);
    LoadLayout(layout_);
    SetOnInput([this](const u64 down, const u64, const u64, const pu::ui::TouchPoint touch) {
        on_input(down, touch);
    });
    AddRenderCallback([this] {
        poll_share_worker();
        advance_automation();
    });
}

void GalleryApplication::advance_automation() {
#ifdef NXGALLERY_AUTOMATION_BUILD
    constexpr char kSendTrigger[] = "sdmc:/switch/nxgallery/automation-send";
    ++automation_frame_;
    if (automation_frame_ == 120 && controller_.screen() == Screen::Grid) {
        controller_.handle(Action::Confirm);
    } else if (automation_frame_ == 300 && controller_.screen() == Screen::Viewer) {
        open_chat_picker();
    } else if (automation_frame_ > 300 && !automation_send_started_ &&
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
    std::vector<TelegramChat> chats;
    if (bot_) {
        BotResult result = bot_->refresh_chats(chats);
        status_ = std::move(result.message);
    } else {
        status_ = "Telegram is not configured";
    }
    controller_.set_chats(std::move(chats));
    controller_.handle(Action::Share);
}

void GalleryApplication::start_share(ShareRequest request) {
    if (!bot_ || share_worker_.joinable()) {
        controller_.finish_share(false, "Telegram is unavailable");
        return;
    }
    share_worker_ = std::thread([this, request = std::move(request)]() mutable {
        BotResult result = bot_->send_media(request.media, request.chat);
        std::lock_guard<std::mutex> lock(share_mutex_);
        share_result_ = std::move(result);
    });
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
    if ((down & HidNpadButton_Plus) != 0 && controller_.screen() != Screen::Sending) { Close(); return; }
    if (controller_.screen() == Screen::Sending) return;
    if ((down & HidNpadButton_X) != 0 && controller_.screen() == Screen::Viewer) { open_chat_picker(); return; }
    if ((down & HidNpadButton_Y) != 0 && controller_.screen() == Screen::ChatPicker) {
        std::vector<TelegramChat> chats;
        if (bot_) {
            BotResult result = bot_->refresh_chats(chats);
            status_ = std::move(result.message);
            controller_.set_chats(std::move(chats));
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
