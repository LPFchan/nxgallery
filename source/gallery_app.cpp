#include <nxgallery/gallery_app.hpp>
#include <nxgallery/horizon_album.hpp>
#include <nxgallery/release_update.hpp>
#include <nxgallery/video_player.hpp>

#include "qrcodegen.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NXGALLERY_VERSION
#define NXGALLERY_VERSION "0.0.0"
#endif

namespace nxgallery {
namespace {

constexpr std::int32_t kWidth = 1280;
constexpr std::int32_t kHeight = 720;

// Horizon light-theme palette, matched to the stock Album applet.
constexpr pu::ui::Color kBackground{235, 235, 235, 255};
constexpr pu::ui::Color kInk{45, 45, 45, 255};
constexpr pu::ui::Color kMuted{125, 125, 130, 255};
constexpr pu::ui::Color kRule{45, 45, 45, 255};
constexpr pu::ui::Color kDialogFace{240, 240, 240, 255};
constexpr pu::ui::Color kDialogRule{201, 201, 205, 255};
constexpr pu::ui::Color kDialogAction{50, 80, 240, 255};
constexpr pu::ui::Color kRowFace{252, 252, 252, 255};
constexpr pu::ui::Color kBlack{0, 0, 0, 255};
constexpr pu::ui::Color kWhite{255, 255, 255, 255};
constexpr pu::ui::Color kOverlayBar{13, 13, 15, 205};
constexpr pu::ui::Color kOverlayInk{225, 225, 230, 255};
constexpr pu::ui::Color kAccent{0, 195, 227, 255};
constexpr pu::ui::Color kSuccess{40, 155, 80, 255};
constexpr pu::ui::Color kFailure{200, 55, 65, 255};

// Album grid: 4 columns x 3 rows of 16:9 cells between the header and footer rules.
constexpr std::int32_t kHeaderRuleY = 88;
constexpr std::int32_t kFooterRuleY = 647;
constexpr std::int32_t kRuleInsetX = 30;
constexpr std::int32_t kGridX = 40;
constexpr std::int32_t kGridY = 104;
constexpr std::int32_t kCellWidth = 294;
constexpr std::int32_t kCellHeight = 165;
constexpr std::int32_t kCellStrideX = 302;
constexpr std::int32_t kCellStrideY = 173;
constexpr std::size_t kThumbnailPageSize =
    GalleryController::kGridColumns * GalleryController::kVisibleRows;
constexpr std::size_t kThumbnailCachePages = 4;
constexpr std::size_t kMaximumThumbnailTextures =
    kThumbnailPageSize * kThumbnailCachePages;

// Viewer bottom controls and video timeline.
constexpr std::int32_t kViewerBarHeight = 64;
constexpr std::int32_t kTimelineX = 40;
constexpr std::int32_t kTimelineY = 634;
constexpr std::int32_t kTimelineWidth = 980;

// Chat picker, sized like a Horizon system dialog.
constexpr std::int32_t kPickerX = 200;
constexpr std::int32_t kPickerY = 100;
constexpr std::int32_t kPickerWidth = 880;
constexpr std::int32_t kPickerHeight = 520;
constexpr std::int32_t kPickerRowX = 232;
constexpr std::int32_t kPickerRowY = 200;
constexpr std::int32_t kPickerRowWidth = 816;
constexpr std::int32_t kPickerRowHeight = 52;
constexpr std::int32_t kPickerRowStride = 56;
constexpr std::size_t kPickerVisibleRows = 6;
constexpr std::int32_t kPickerButtonY = 548;
constexpr std::int32_t kPickerButtonHeight = 72;

// Sending and result dialogs share one footprint.
constexpr std::int32_t kDialogX = 280;
constexpr std::int32_t kDialogY = 220;
constexpr std::int32_t kDialogWidth = 720;
constexpr std::int32_t kDialogHeight = 280;
constexpr std::int32_t kDialogButtonY = 428;
constexpr std::int32_t kDialogButtonHeight = 72;
constexpr std::int32_t kDialogRadius = 6;

// Touch: a press that travels at least this far before release is a swipe.
constexpr std::int32_t kSwipeThreshold = 60;

// Screen transitions: ~200 ms at 60 FPS. Grid<->Viewer and viewer browsing
// dip through black; dialogs rise this many pixels while their dim fades in.
constexpr std::uint32_t kTransitionFrames = 12;
constexpr std::int32_t kDialogRiseHeight = 26;
constexpr std::int32_t kViewerNavigationOffset = kWidth / 5;

double ease_out_cubic(double t) {
    const double inverse = 1.0 - t;
    return 1.0 - inverse * inverse * inverse;
}

// Token setup overlay: reuses the chat-picker dialog footprint, QR panel on
// the left, instructions on the right.
constexpr char kTelegramConfigPath[] = "sdmc:/switch/nxgallery/telegram-bot.conf";
constexpr char kRawAlbumPath[] = "sdmc:/Nintendo/Album";
#ifdef NXGALLERY_AUTOMATION_BUILD
constexpr char kAutomationUpdateMarker[] =
    "sdmc:/switch/nxgallery/automation-update";
#endif
constexpr std::uint16_t kSetupPort = 8135;
constexpr std::int32_t kSetupQrX = 240;
constexpr std::int32_t kSetupQrY = 176;
constexpr std::int32_t kSetupQrPanel = 320;
constexpr std::int32_t kSetupTextX = 600;

enum class HintTag { View, PlayPause, Prev, Next, Share, Back, MultiSelect, Update };

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

std::string playback_time(std::uint64_t milliseconds) {
    const std::uint64_t seconds = milliseconds / 1000U;
    const std::uint64_t minutes = seconds / 60U;
    const std::uint64_t remainder = seconds % 60U;
    return std::to_string(minutes) + ":" +
        (remainder < 10 ? "0" : "") + std::to_string(remainder);
}

pu::ui::Color with_opacity(pu::ui::Color color, double opacity) {
    color.a = static_cast<std::uint8_t>(color.a * std::clamp(opacity, 0.0, 1.0));
    return color;
}

}  // namespace

class GalleryApplication::GalleryElement final : public pu::ui::elm::Element {
public:
    GalleryElement(GalleryController &controller, std::string &status,
                   VideoPlayer &video_player,
                   std::atomic<std::uint64_t> &transfer_current,
                   std::atomic<std::uint64_t> &transfer_total,
                   std::atomic<bool> &transfer_cancel_requested,
                   bool &setup_active, std::string &setup_url,
                   std::string &setup_notice, bool &setup_fullscreen,
                   bool &album_loading, bool &update_notice_active,
                   std::string &update_notice, bool &update_available,
                   bool &update_installing,
                   std::atomic<std::uint64_t> &update_current,
                   std::atomic<std::uint64_t> &update_total)
        : controller_(controller), status_(status), video_player_(video_player),
          transfer_current_(transfer_current), transfer_total_(transfer_total),
          transfer_cancel_requested_(transfer_cancel_requested),
          setup_active_(setup_active), setup_url_(setup_url),
          setup_notice_(setup_notice), setup_fullscreen_(setup_fullscreen),
          album_loading_(album_loading),
          update_notice_active_(update_notice_active),
          update_notice_(update_notice), update_available_(update_available),
          update_installing_(update_installing),
          update_current_(update_current), update_total_(update_total) {
        thumbnail_states_.resize(controller_.media().size(), ThumbnailState::Unloaded);
        thumbnail_slots_.reserve(std::min(controller_.media().size(),
                                          kMaximumThumbnailTextures));
    }

    ~GalleryElement() override { clear_textures(); }

    s32 GetX() override { return 0; }
    s32 GetY() override { return 0; }
    s32 GetWidth() override { return kWidth; }
    s32 GetHeight() override { return kHeight; }
    void OnInput(const u64, const u64, const u64, const pu::ui::TouchPoint) override {}

    // Which hint label (if any) sits under a touch point rendered last frame.
    std::optional<HintTag> hint_at(std::int32_t x, std::int32_t y) const {
        for (const HintZone &zone : hint_zones_) {
            if (x >= zone.x && x < zone.x + zone.width &&
                y >= zone.y && y < zone.y + zone.height) {
                return zone.tag;
            }
        }
        return std::nullopt;
    }

    void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32, const s32) override {
        if (thumbnail_loading_started_) update_thumbnail_cache();
        text_slot_ = 0;
        hint_zones_.clear();
        ++pulse_frame_;
        advance_transition();
        switch (controller_.screen()) {
            case Screen::Grid:
                render_grid(drawer);
                if (transition_from_ == Screen::ChatPicker && transition_t_ < 1.0) {
                    render_chat_picker(drawer, 1.0 - transition_linear_t_,
                                       static_cast<std::int32_t>(
                                           transition_t_ * kDialogRiseHeight));
                }
                break;
            case Screen::Viewer:
                render_viewer(drawer);
                if (transition_from_ == Screen::ChatPicker && transition_t_ < 1.0) {
                    render_chat_picker(drawer, 1.0 - transition_linear_t_,
                                       static_cast<std::int32_t>(
                                           transition_t_ * kDialogRiseHeight));
                }
                break;
            case Screen::ChatPicker: render_share_background(drawer); render_chat_picker(drawer); break;
            case Screen::Sending: render_share_background(drawer); render_sending(drawer); break;
            case Screen::Result: render_share_background(drawer); render_result(drawer); break;
        }
        if (setup_active_) render_token_setup(drawer);
        else if (update_notice_active_) render_update_notice(drawer);
        render_screen_fade(drawer);
        for (std::size_t index = text_slot_; index < text_slots_.size(); ++index) {
            pu::ui::render::DeleteTexture(text_slots_[index].texture);
        }
        text_slots_.resize(text_slot_);
        thumbnail_loading_started_ = true;
    }

private:
    struct TextSlot { std::string key; pu::sdl2::Texture texture{nullptr}; };
    struct ImageSlot { std::string path; pu::sdl2::Texture texture{nullptr}; };
    enum class ThumbnailState : std::uint8_t { Unloaded, Cached, Failed };
    struct ThumbnailSlot {
        std::size_t media_index{};
        pu::sdl2::Texture texture{nullptr};
        std::uint32_t loaded_frame{};
    };
    struct HintZone {
        std::int32_t x;
        std::int32_t y;
        std::int32_t width;
        std::int32_t height;
        HintTag tag;
    };
    struct HintItem {
        std::string text;
        std::optional<HintTag> tag;
        std::int32_t gap_after;
    };

    void clear_textures() {
        for (auto &slot : text_slots_) pu::ui::render::DeleteTexture(slot.texture);
        for (auto &slot : image_slots_) pu::ui::render::DeleteTexture(slot.texture);
        for (auto &slot : thumbnail_slots_) {
            pu::ui::render::DeleteTexture(slot.texture);
        }
        text_slots_.clear();
        image_slots_.clear();
        thumbnail_slots_.clear();
        thumbnail_states_.clear();
    }

    bool load_thumbnail(std::size_t media_index) {
        const auto &media = controller_.media();
        if (media_index >= media.size() ||
            thumbnail_states_[media_index] != ThumbnailState::Unloaded) {
            return false;
        }

        std::string resolved_path;
        std::string error;
        pu::sdl2::Texture texture = nullptr;
        if (materialize_thumbnail_path(media[media_index], resolved_path, error)) {
            texture = pu::ui::render::LoadImageFromFile(resolved_path);
            if (texture == nullptr) error = "Could not decode capture thumbnail";
        }
        if (texture == nullptr) {
            thumbnail_states_[media_index] = ThumbnailState::Failed;
            if (status_.empty()) {
                status_ = error.empty() ? "Some capture thumbnails could not be loaded" :
                                          std::move(error);
            }
            return true;
        }

        thumbnail_slots_.push_back({media_index, texture, pulse_frame_});
        thumbnail_states_[media_index] = ThumbnailState::Cached;
        return true;
    }

    void update_thumbnail_cache() {
        const auto &media = controller_.media();
        if (thumbnail_states_.size() != media.size()) {
            for (auto &slot : thumbnail_slots_) {
                pu::ui::render::DeleteTexture(slot.texture);
            }
            thumbnail_slots_.clear();
            thumbnail_states_.assign(media.size(), ThumbnailState::Unloaded);
            thumbnail_slots_.reserve(std::min(media.size(), kMaximumThumbnailTextures));
        }
        if (media.empty()) return;

        const std::size_t page_count =
            (media.size() + kThumbnailPageSize - 1) / kThumbnailPageSize;
        const std::size_t current_page =
            controller_.grid_page_start() / kThumbnailPageSize;
        const std::size_t maximum_first_page =
            page_count > kThumbnailCachePages ? page_count - kThumbnailCachePages : 0;
        const std::size_t preferred_first_page =
            current_page > 0 ? current_page - 1 : 0;
        const std::size_t first_page =
            std::min(preferred_first_page, maximum_first_page);
        const std::size_t first_index = first_page * kThumbnailPageSize;
        const std::size_t end_index =
            std::min(media.size(), first_index + kMaximumThumbnailTextures);

        auto slot = thumbnail_slots_.begin();
        while (slot != thumbnail_slots_.end()) {
            if (slot->media_index < first_index || slot->media_index >= end_index) {
                pu::ui::render::DeleteTexture(slot->texture);
                thumbnail_states_[slot->media_index] = ThumbnailState::Unloaded;
                slot = thumbnail_slots_.erase(slot);
            } else {
                ++slot;
            }
        }

        const std::size_t visible_start = controller_.grid_page_start();
        const std::size_t visible_end =
            std::min(media.size(), visible_start + kThumbnailPageSize);
        auto load_first_missing = [this](std::size_t start, std::size_t end) {
            for (std::size_t index = start; index < end; ++index) {
                if (thumbnail_states_[index] == ThumbnailState::Unloaded) {
                    load_thumbnail(index);
                    return true;
                }
            }
            return false;
        };

        // One decode per frame spreads refill work across navigation frames.
        if (load_first_missing(visible_start, visible_end)) return;
        if (load_first_missing(visible_end, end_index)) return;
        load_first_missing(first_index, visible_start);
    }

    pu::sdl2::Texture thumbnail(std::size_t media_index) const {
        auto found = std::find_if(thumbnail_slots_.begin(), thumbnail_slots_.end(),
                                  [media_index](const ThumbnailSlot &slot) {
                                      return slot.media_index == media_index;
                                  });
        return found == thumbnail_slots_.end() ? nullptr : found->texture;
    }

    std::uint8_t thumbnail_alpha(std::size_t media_index) const {
        constexpr std::uint32_t kThumbnailFadeFrames = 12;
        auto found = std::find_if(thumbnail_slots_.begin(), thumbnail_slots_.end(),
                                  [media_index](const ThumbnailSlot &slot) {
                                      return slot.media_index == media_index;
                                  });
        if (found == thumbnail_slots_.end()) return 0;
        const std::uint32_t age = pulse_frame_ - found->loaded_frame;
        return static_cast<std::uint8_t>(
            std::min<std::uint32_t>(255, age * 255 / kThumbnailFadeFrames));
    }

    pu::sdl2::Texture text_texture(const std::string &value, std::int32_t size,
                                   pu::ui::Color color) {
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
        return slot.texture;
    }

    void text(pu::ui::render::Renderer::Ref &drawer, const std::string &value,
              std::int32_t size, pu::ui::Color color, std::int32_t x,
              std::int32_t y) {
        pu::sdl2::Texture texture = text_texture(value, size, color);
        if (texture != nullptr) {
            drawer->RenderTexture(texture, x, y,
                                  pu::ui::render::TextureRenderOptions(
                                      render_alpha_, {}, {}, {}, {}, {}));
        }
    }

    void text_right(pu::ui::render::Renderer::Ref &drawer, const std::string &value,
                    std::int32_t size, pu::ui::Color color, std::int32_t right_x,
                    std::int32_t y) {
        pu::sdl2::Texture texture = text_texture(value, size, color);
        if (texture == nullptr) return;
        drawer->RenderTexture(texture,
                              right_x - pu::ui::render::GetTextureWidth(texture), y,
                              pu::ui::render::TextureRenderOptions(
                                  render_alpha_, {}, {}, {}, {}, {}));
    }

    void text_center(pu::ui::render::Renderer::Ref &drawer, const std::string &value,
                     std::int32_t size, pu::ui::Color color, std::int32_t center_x,
                     std::int32_t y) {
        pu::sdl2::Texture texture = text_texture(value, size, color);
        if (texture == nullptr) return;
        drawer->RenderTexture(texture,
                              center_x - pu::ui::render::GetTextureWidth(texture) / 2, y,
                              pu::ui::render::TextureRenderOptions(
                                  render_alpha_, {}, {}, {}, {}, {}));
    }

    // Renders hint labels right-aligned, recording a full-bar-height touch zone
    // for every tagged label.
    void hints(pu::ui::render::Renderer::Ref &drawer, const std::vector<HintItem> &items,
               std::int32_t right_x, std::int32_t text_y, std::int32_t size,
               pu::ui::Color color, std::int32_t zone_y, std::int32_t zone_height) {
        std::vector<pu::sdl2::Texture> textures;
        std::int32_t total = 0;
        for (std::size_t index = 0; index < items.size(); ++index) {
            textures.push_back(text_texture(items[index].text, size, color));
            if (textures.back() != nullptr) {
                total += pu::ui::render::GetTextureWidth(textures.back());
            }
            if (index + 1 < items.size()) total += items[index].gap_after;
        }
        std::int32_t x = right_x - total;
        for (std::size_t index = 0; index < items.size(); ++index) {
            const pu::sdl2::Texture texture = textures[index];
            if (texture == nullptr) continue;
            const std::int32_t width = pu::ui::render::GetTextureWidth(texture);
            drawer->RenderTexture(texture, x, text_y,
                                  pu::ui::render::TextureRenderOptions(
                                      render_alpha_, {}, {}, {}, {}, {}));
            if (items[index].tag) {
                hint_zones_.push_back({x - 24, zone_y, width + 48, zone_height,
                                       *items[index].tag});
            }
            x += width + items[index].gap_after;
        }
    }

    // Restarts the transition clock whenever the visible screen changes, or
    // the viewer moves to another capture, and eases progress toward 1.
    void advance_transition() {
        const Screen screen = controller_.screen();
        const std::size_t media_index = controller_.selected_media_index();
        const bool update_notice_opened =
            update_notice_active_ && !shown_update_notice_;
        shown_update_notice_ = update_notice_active_;
        if (update_notice_opened) {
            transition_from_ = shown_screen_;
            transition_frame_ = 0;
            viewer_navigation_transition_ = false;
        } else if (screen != shown_screen_) {
            transition_from_ = shown_screen_;
            shown_screen_ = screen;
            transition_frame_ = 0;
            viewer_navigation_transition_ = false;
        } else if (screen == Screen::Viewer && media_index != shown_media_index_) {
            transition_from_ = Screen::Viewer;
            transition_frame_ = 0;
            previous_media_index_ = shown_media_index_;
            navigation_direction_ = media_index > shown_media_index_ ? 1 : -1;
            const auto &media = controller_.media();
            viewer_navigation_transition_ = previous_media_index_ < media.size() &&
                media_index < media.size();
        }
        shown_media_index_ = media_index;
        transition_linear_t_ =
            static_cast<double>(transition_frame_) / kTransitionFrames;
        transition_t_ = ease_out_cubic(transition_linear_t_);
        if (transition_frame_ < kTransitionFrames) ++transition_frame_;
    }

    // Fullscreen dip-through-black for Grid<->Viewer. Capture browsing uses a
    // direct directional crossfade so the viewer chrome never dims or flickers.
    void render_screen_fade(pu::ui::render::Renderer::Ref &drawer) {
        if (transition_t_ >= 1.0) return;
        const bool fades =
            (shown_screen_ == Screen::Grid && transition_from_ == Screen::Viewer) ||
            (shown_screen_ == Screen::Viewer &&
             (transition_from_ == Screen::Grid ||
              (transition_from_ == Screen::Viewer && !viewer_navigation_transition_)));
        if (!fades) return;
        const std::uint8_t alpha =
            static_cast<std::uint8_t>((1.0 - transition_t_) * 255.0);
        drawer->RenderRectangleFill({0, 0, 0, alpha}, 0, 0, kWidth, kHeight);
    }

    // How far a dialog still sits below its resting position this frame.
    std::int32_t dialog_rise() const {
        return static_cast<std::int32_t>((1.0 - transition_t_) * kDialogRiseHeight);
    }

    // The dim layer fades in only when a dialog opens over Grid or Viewer;
    // between two dialog screens it stays opaque so the backdrop never flashes.
    double dim_progress() const {
        const bool from_dialog = transition_from_ == Screen::ChatPicker ||
            transition_from_ == Screen::Sending ||
            transition_from_ == Screen::Result;
        return from_dialog ? 1.0 : transition_t_;
    }

    // Stock Horizon selections breathe between two cyans on a ~1.5 s cycle.
    pu::ui::Color pulse_color() const {
        const std::uint32_t phase = pulse_frame_ % 90U;
        const double t = phase < 45U ? phase / 45.0 : (90U - phase) / 45.0;
        auto mix = [t](std::uint8_t from, std::uint8_t to) {
            return static_cast<std::uint8_t>(from + (to - from) * t);
        };
        return {mix(0, 90), mix(195, 225), mix(227, 255), 255};
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

    void fitted_image(pu::ui::render::Renderer::Ref &drawer, pu::sdl2::Texture texture,
                      std::int32_t x, std::int32_t y, std::int32_t width,
                      std::int32_t height, std::uint8_t alpha = 255) {
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
                              pu::ui::render::TextureRenderOptions(alpha, render_width,
                                  render_height, {}, {}, {}));
    }

    void dialog_face(pu::ui::render::Renderer::Ref &drawer, std::uint8_t dim_alpha,
                     std::int32_t x, std::int32_t y, std::int32_t width,
                     std::int32_t height, std::int32_t button_row_y,
                     double opacity = 1.0) {
        drawer->RenderRectangleFill(
            {0, 0, 0, static_cast<std::uint8_t>(
                dim_alpha * dim_progress() * opacity)},
            0, 0, kWidth, kHeight);
        drawer->RenderRoundedRectangleFill(with_opacity(kDialogFace, opacity),
                                           x, y, width, height, kDialogRadius);
        drawer->RenderRectangleFill(with_opacity(kDialogRule, opacity),
                                    x, button_row_y, width, 1);
    }

    void render_grid(pu::ui::render::Renderer::Ref &drawer) {
        drawer->RenderRectangleFill(kBackground, 0, 0, kWidth, kHeight);
        text(drawer, controller_.multi_select_active() ? "Select captures" : "Album",
             29, kInk, 60, 27);
        drawer->RenderRectangleFill(kRule, kRuleInsetX, kHeaderRuleY,
                                    kWidth - 2 * kRuleInsetX, 1);
        drawer->RenderRectangleFill(kRule, kRuleInsetX, kFooterRuleY,
                                    kWidth - 2 * kRuleInsetX, 1);
        const auto &media = controller_.media();
        if (controller_.multi_select_active()) {
            text_right(drawer,
                       std::to_string(controller_.selected_media_count()) + " / " +
                           std::to_string(GalleryController::kMaximumMultiSelect) +
                           " selected",
                       18, kMuted, kWidth - 60, 40);
        } else if (!media.empty()) {
            text_right(drawer, std::to_string(media.size()) + " captures", 18,
                       kMuted, kWidth - 60, 40);
        } else if (album_loading_) {
            text_center(drawer, "Loading captures...", 25, kInk,
                        kWidth / 2, 320);
            text_center(drawer, "The gallery will fill in as captures become ready.",
                        18, kMuted, kWidth / 2, 366);
        } else {
            text_center(drawer, "There are no screenshots or videos.", 25, kInk,
                        kWidth / 2, 320);
            text_center(drawer, "Captures taken with the Capture Button will appear here.",
                        18, kMuted, kWidth / 2, 366);
        }
        const std::size_t start = controller_.grid_page_start();
        for (std::size_t visible = 0;
             visible < GalleryController::kGridColumns * GalleryController::kVisibleRows;
             ++visible) {
            const std::size_t index = start + visible;
            if (index >= media.size()) break;
            const std::int32_t column = static_cast<std::int32_t>(visible % GalleryController::kGridColumns);
            const std::int32_t row = static_cast<std::int32_t>(visible / GalleryController::kGridColumns);
            const std::int32_t x = kGridX + column * kCellStrideX;
            const std::int32_t y = kGridY + row * kCellStrideY;
            drawer->RenderRectangleFill(kBlack, x, y, kCellWidth, kCellHeight);
            fitted_image(drawer, thumbnail(index), x, y, kCellWidth, kCellHeight,
                         thumbnail_alpha(index));
            if (media[index].kind == MediaKind::Video) {
                drawer->RenderRoundedRectangleFill({0, 0, 0, 170}, x + kCellWidth - 58,
                                                   y + kCellHeight - 32, 50, 24, 4);
                text(drawer, "▶", 16, kWhite, x + kCellWidth - 45, y + kCellHeight - 30);
            }
            if (controller_.multi_select_active()) {
                const bool marked = controller_.is_media_selected(index);
                if (marked) {
                    render_outline(drawer, kAccent, x + 2, y + 2,
                                   kCellWidth - 4, kCellHeight - 4, 5);
                }
                drawer->RenderRoundedRectangleFill(
                    marked ? kAccent : pu::ui::Color{0, 0, 0, 150},
                    x + 12, y + 12, 32, 32, 16);
                render_outline(drawer, kWhite, x + 18, y + 18, 20, 20, 2);
                if (marked) {
                    drawer->RenderRectangleFill(kWhite, x + 22, y + 22, 12, 12);
                }
            }
        }
        // Drawn after every cell so the thick border is never clipped by a
        // neighbouring thumbnail.
        const std::size_t selected = controller_.selected_media_index();
        if (!media.empty() && selected >= start &&
            selected < start + GalleryController::kGridColumns * GalleryController::kVisibleRows) {
            const std::size_t visible = selected - start;
            const std::int32_t x = kGridX +
                static_cast<std::int32_t>(visible % GalleryController::kGridColumns) * kCellStrideX;
            const std::int32_t y = kGridY +
                static_cast<std::int32_t>(visible / GalleryController::kGridColumns) * kCellStrideY;
            render_outline(drawer, kWhite, x - 3, y - 3,
                           kCellWidth + 6, kCellHeight + 6, 3);
            render_outline(drawer, pulse_color(), x - 9, y - 9,
                           kCellWidth + 18, kCellHeight + 18, 6);
        }
        std::vector<HintItem> grid_hints;
        if (update_available_) {
            hints(drawer, {{" Update", HintTag::Update, 0}},
                  210, 663, 22, kInk, kFooterRuleY + 1,
                  kHeight - kFooterRuleY - 1);
        }
        grid_hints.push_back({controller_.multi_select_active() ? " Select" : " View",
                              HintTag::View, 36});
        grid_hints.push_back({" Share", HintTag::Share, 36});
        grid_hints.push_back({controller_.multi_select_active() ? " Done" : " Select",
                              HintTag::MultiSelect, 0});
        hints(drawer, grid_hints,
              kWidth - 60, 663, 22, kInk, kFooterRuleY + 1, kHeight - kFooterRuleY - 1);
    }

    void render_share_background(pu::ui::render::Renderer::Ref &drawer) {
        if (controller_.share_origin() == Screen::Grid) render_grid(drawer);
        else render_viewer(drawer);
    }

    void render_qr(pu::ui::render::Renderer::Ref &drawer, const std::string &content,
                   std::int32_t x, std::int32_t y, std::int32_t panel) {
        drawer->RenderRoundedRectangleFill(kWhite, x, y, panel, panel, 8);
        if (content.empty()) {
            text_center(drawer, "QR unavailable", 18, kFailure,
                        x + panel / 2, y + panel / 2 - 10);
            return;
        }
        if (qr_key_ != content) {
            qr_key_ = content;
            qr_size_ = 0;
            std::vector<std::uint8_t> qrcode(qrcodegen_BUFFER_LEN_MAX);
            std::vector<std::uint8_t> scratch(qrcodegen_BUFFER_LEN_MAX);
            if (qrcodegen_encodeText(content.c_str(), scratch.data(), qrcode.data(),
                                     qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                     qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO,
                                     true)) {
                qr_size_ = qrcodegen_getSize(qrcode.data());
                qr_modules_.assign(static_cast<std::size_t>(qr_size_) * qr_size_, 0);
                for (int row = 0; row < qr_size_; ++row) {
                    for (int column = 0; column < qr_size_; ++column) {
                        qr_modules_[static_cast<std::size_t>(row) * qr_size_ + column] =
                            qrcodegen_getModule(qrcode.data(), column, row) ? 1 : 0;
                    }
                }
            }
        }
        if (qr_size_ <= 0) {
            text_center(drawer, "QR unavailable", 18, kFailure,
                        x + panel / 2, y + panel / 2 - 10);
            return;
        }
        const std::int32_t scale = std::max<std::int32_t>(1, (panel - 32) / qr_size_);
        const std::int32_t origin_x = x + (panel - scale * qr_size_) / 2;
        const std::int32_t origin_y = y + (panel - scale * qr_size_) / 2;
        for (int row = 0; row < qr_size_; ++row) {
            for (int column = 0; column < qr_size_; ++column) {
                if (qr_modules_[static_cast<std::size_t>(row) * qr_size_ + column] == 0) continue;
                drawer->RenderRectangleFill(kBlack, origin_x + column * scale,
                                            origin_y + row * scale, scale, scale);
            }
        }
    }

    void render_token_setup(pu::ui::render::Renderer::Ref &drawer) {
        if (setup_fullscreen_) {
            drawer->RenderRectangleFill(kBackground, 0, 0, kWidth, kHeight);
            text_center(drawer, "Connect Telegram", 34, kInk, kWidth / 2, 54);
            text_center(drawer, "Create a bot with @BotFather, then connect NX Gallery.",
                        19, kMuted, kWidth / 2, 104);
            render_qr(drawer, setup_url_, 170, 150, 390);
            text(drawer, "1. Keep the Switch and phone on the same Wi-Fi.",
                 20, kInk, 630, 192);
            text(drawer, "2. Scan the QR code with your phone.",
                 20, kInk, 630, 246);
            text(drawer, "3. Paste the bot token and send it.",
                 20, kInk, 630, 300);
            text(drawer, "Browser address", 17, kMuted, 630, 374);
            text(drawer, clipped(setup_url_, 48), 18, kInk, 630, 406);
            if (!setup_notice_.empty()) {
                text(drawer, clipped(setup_notice_, 54), 18, kMuted, 630, 466);
            }
            text_center(drawer, " Continue without Telegram", 22, kDialogAction,
                        kWidth / 2, 660);
            return;
        }
        dialog_face(drawer, 150, kPickerX, kPickerY, kPickerWidth, kPickerHeight,
                    kPickerButtonY);
        text_center(drawer, "Set up Telegram sharing", 25, kInk, kWidth / 2, 124);
        render_qr(drawer, setup_url_, kSetupQrX, kSetupQrY, kSetupQrPanel);
        text(drawer, "1. Create a bot with @BotFather.", 20, kInk, kSetupTextX, 192);
        text(drawer, "2. Scan the code with your phone.", 20, kInk, kSetupTextX, 240);
        text(drawer, "Phone and Switch: same Wi-Fi.", 17, kMuted, kSetupTextX + 24, 272);
        text(drawer, "3. Paste the bot token and send.", 20, kInk, kSetupTextX, 320);
        text(drawer, "Or open this address in a browser:", 17, kMuted, kSetupTextX, 384);
        text(drawer, clipped(setup_url_, 42), 18, kInk, kSetupTextX, 412);
        if (!setup_notice_.empty()) {
            text_center(drawer, clipped(setup_notice_, 64), 18, kMuted,
                        kWidth / 2, 514);
        }
        text_center(drawer, " Cancel", 24, kDialogAction, kWidth / 2,
                    kPickerButtonY + 20);
    }

    void render_viewer(pu::ui::render::Renderer::Ref &drawer) {
        drawer->RenderRectangleFill(kBlack, 0, 0, kWidth, kHeight);
        const auto &media = controller_.media();
        const bool video_selected = !media.empty() &&
            media[controller_.selected_media_index()].kind == MediaKind::Video;
        if (!media.empty()) {
            const std::size_t selected_index = controller_.selected_media_index();
            const MediaItem &selected = media[selected_index];
            if (viewer_navigation_transition_ && transition_t_ < 1.0 &&
                previous_media_index_ < media.size()) {
                const double motion_progress = transition_t_;
                const double opacity_progress = transition_linear_t_;
                const std::int32_t previous_x = static_cast<std::int32_t>(
                    -navigation_direction_ * motion_progress * kViewerNavigationOffset);
                const std::int32_t selected_x = static_cast<std::int32_t>(
                    navigation_direction_ * (1.0 - motion_progress) *
                    kViewerNavigationOffset);
                const std::uint8_t previous_alpha = static_cast<std::uint8_t>(
                    (1.0 - opacity_progress) * 255.0);
                const std::uint8_t selected_alpha = static_cast<std::uint8_t>(
                    opacity_progress * 255.0);
                const pu::sdl2::Texture previous_texture =
                    media[previous_media_index_].kind == MediaKind::Photo ?
                    image(media[previous_media_index_].path) :
                    thumbnail(previous_media_index_);
                const pu::sdl2::Texture selected_texture =
                    selected.kind == MediaKind::Photo ? image(selected.path) :
                                                        thumbnail(selected_index);
                fitted_image(drawer, previous_texture,
                             previous_x, 0, kWidth, kHeight, previous_alpha);
                fitted_image(drawer, selected_texture, selected_x, 0,
                             kWidth, kHeight, selected_alpha);
            } else if (selected.kind == MediaKind::Photo) {
                fitted_image(drawer, image(selected.path), 0, 0, kWidth, kHeight);
            } else {
                pu::sdl2::Texture frame = video_player_.texture();
                fitted_image(drawer, frame != nullptr ? frame :
                             thumbnail(selected_index),
                             0, 0, kWidth, kHeight);
            }
        }

        drawer->RenderRectangleFill(kOverlayBar, 0, kHeight - kViewerBarHeight,
                                    kWidth, kViewerBarHeight);
        if (!media.empty()) {
            text(drawer, std::to_string(controller_.selected_media_index() + 1) +
                 " / " + std::to_string(media.size()), 18, kOverlayInk, 40, 678);
        }
        std::vector<HintItem> viewer_hints;
        if (video_selected) viewer_hints.push_back({" Play/Pause", HintTag::PlayPause, 36});
        viewer_hints.push_back({"", HintTag::Prev, 8});
        viewer_hints.push_back({"", HintTag::Next, 10});
        viewer_hints.push_back({"Browse", HintTag::Next, 36});
        viewer_hints.push_back({" Share", HintTag::Share, 36});
        viewer_hints.push_back({" Back", HintTag::Back, 0});
        hints(drawer, viewer_hints, kWidth - 40, 676, 20, kWhite,
              kHeight - kViewerBarHeight, kViewerBarHeight);

        if (video_selected && controller_.screen() == Screen::Viewer) {
            const std::uint64_t position = video_player_.position_ms();
            const std::uint64_t duration = video_player_.duration_ms();
            drawer->RenderRoundedRectangleFill({255, 255, 255, 60}, kTimelineX,
                                               kTimelineY, kTimelineWidth, 8, 4);
            if (duration > 0 && position > 0) {
                const std::int32_t fill_width = std::max<std::int32_t>(
                    8, static_cast<std::int32_t>(
                        std::min<std::uint64_t>(duration, position) * kTimelineWidth /
                        duration));
                drawer->RenderRoundedRectangleFill(kAccent, kTimelineX, kTimelineY,
                                                   fill_width, 8, 4);
            }
            text_right(drawer, playback_time(position) + " / " + playback_time(duration),
                       18, kWhite, kWidth - 40, 626);
            const std::string playback_status = video_player_.status();
            if (!playback_status.empty()) {
                text(drawer, clipped(playback_status, 72), 17, kOverlayInk, 40, 600);
            }
        }
    }

    void render_chat_picker(pu::ui::render::Renderer::Ref &drawer,
                            double opacity = 1.0,
                            std::optional<std::int32_t> rise_override = std::nullopt) {
        const std::int32_t rise = rise_override.value_or(dialog_rise());
        const std::uint8_t previous_render_alpha = render_alpha_;
        render_alpha_ = static_cast<std::uint8_t>(255.0 * opacity);
        dialog_face(drawer, 140, kPickerX, kPickerY + rise, kPickerWidth,
                    kPickerHeight, kPickerButtonY + rise, opacity);
        text_center(drawer, "Share to Telegram", 25, kInk, kWidth / 2, 128 + rise);
        text_right(drawer, " Bot Setup", 18, kMuted,
                   kPickerX + kPickerWidth - 24, 130 + rise);
        if (!status_.empty()) {
            text_center(drawer, clipped(status_, 70), 18, kMuted, kWidth / 2, 166 + rise);
        }
        const auto &chats = controller_.chats();
        if (chats.empty()) {
            text_center(drawer, "No destinations found.", 23, kInk, kWidth / 2, 300 + rise);
            text_center(drawer, "Message the bot or add a chat=ID|Title entry.",
                        18, kMuted, kWidth / 2, 344 + rise);
        }
        const std::size_t selected = controller_.selected_chat_index();
        const std::size_t start = selected >= kPickerVisibleRows
            ? selected - (kPickerVisibleRows - 1) : 0;
        for (std::size_t row = 0; row < kPickerVisibleRows && start + row < chats.size(); ++row) {
            const std::size_t index = start + row;
            const std::int32_t row_y = kPickerRowY + rise +
                static_cast<std::int32_t>(row) * kPickerRowStride;
            if (index == selected) {
                drawer->RenderRoundedRectangleFill(with_opacity(kRowFace, opacity),
                                                   kPickerRowX, row_y,
                                                   kPickerRowWidth, kPickerRowHeight, 6);
                const pu::ui::Color pulse = with_opacity(pulse_color(), opacity);
                for (std::int32_t inset = 0; inset < 3; ++inset) {
                    drawer->RenderRoundedRectangle(pulse, kPickerRowX - inset,
                                                   row_y - inset,
                                                   kPickerRowWidth + 2 * inset,
                                                   kPickerRowHeight + 2 * inset, 6);
                }
            } else if (row + 1 < kPickerVisibleRows && index + 1 < chats.size()) {
                drawer->RenderRectangleFill(with_opacity({215, 215, 218, 255}, opacity),
                                            kPickerRowX,
                                            row_y + kPickerRowHeight + 1,
                                            kPickerRowWidth, 1);
            }
            text(drawer, clipped(chats[index].title, 44), 23, kInk,
                 kPickerRowX + 16, row_y + 11);
        }
        const std::int32_t third = kPickerWidth / 3;
        drawer->RenderRectangleFill(with_opacity(kDialogRule, opacity),
                                    kPickerX + third,
                                    kPickerButtonY + rise, 1, kPickerButtonHeight);
        drawer->RenderRectangleFill(with_opacity(kDialogRule, opacity),
                                    kPickerX + 2 * third,
                                    kPickerButtonY + rise, 1, kPickerButtonHeight);
        const std::int32_t label_y = kPickerButtonY + 20 + rise;
        text_center(drawer, " Cancel", 24, kDialogAction,
                    kPickerX + third / 2, label_y);
        text_center(drawer, " Refresh", 24, kDialogAction,
                    kPickerX + third + third / 2, label_y);
        text_center(drawer, " Send", 24, kDialogAction,
                    kPickerX + 2 * third + third / 2, label_y);
        render_alpha_ = previous_render_alpha;
    }

    void render_progress_panel(pu::ui::render::Renderer::Ref &drawer,
                               const std::string &title,
                               const std::string &preparing,
                               const std::string &button,
                               std::uint64_t current, std::uint64_t total,
                               std::int32_t rise) {
        dialog_face(drawer, 170, kDialogX, kDialogY + rise, kDialogWidth,
                    kDialogHeight, kDialogButtonY + rise);
        text_center(drawer, title, 25, kInk, kWidth / 2, 258 + rise);
        const std::uint64_t percent = total > 0
            ? std::min<std::uint64_t>(100, current * 100 / total) : 0;
        constexpr std::int32_t bar_x = 360;
        constexpr std::int32_t bar_width = 560;
        drawer->RenderRoundedRectangleFill({205, 205, 208, 255}, bar_x, 330 + rise,
                                           bar_width, 12, 6);
        if (percent > 0) {
            const std::int32_t fill_width = std::max<std::int32_t>(
                12, static_cast<std::int32_t>(percent * bar_width / 100));
            drawer->RenderRoundedRectangleFill(kAccent, bar_x, 330 + rise,
                                               fill_width, 12, 6);
        }
        text_center(drawer, total > 0 ? std::to_string(percent) + "%" : preparing,
                    18, kMuted, kWidth / 2, 358 + rise);
        text_center(drawer, button, 24,
                    button.empty() ? kMuted : kDialogAction,
                    kWidth / 2, kDialogButtonY + 20 + rise);
    }

    void render_sending(pu::ui::render::Renderer::Ref &drawer) {
        const bool cancelling = transfer_cancel_requested_.load();
        render_progress_panel(
            drawer, cancelling ? "Cancelling transfer..." : "Sending to Telegram...",
            "Preparing upload...",
            cancelling ? "Waiting for Telegram to stop" : " Cancel",
            transfer_current_.load(), transfer_total_.load(), dialog_rise());
    }

    void render_result(pu::ui::render::Renderer::Ref &drawer) {
        const std::int32_t rise = dialog_rise();
        dialog_face(drawer, 170, kDialogX, kDialogY + rise, kDialogWidth,
                    kDialogHeight, kDialogButtonY + rise);
        const bool success = controller_.share_succeeded();
        text_center(drawer, success ? "Shared" : "Could not share", 29,
                    success ? kSuccess : kFailure, kWidth / 2, 252 + rise);
        text_center(drawer, clipped(controller_.result_message(), 58), 20, kInk,
                    kWidth / 2, 322 + rise);
        text_center(drawer, "OK", 24, kDialogAction, kWidth / 2,
                    kDialogButtonY + 20 + rise);
    }

    void render_update_notice(pu::ui::render::Renderer::Ref &drawer) {
        const std::int32_t rise = dialog_rise();
        if (update_installing_) {
            render_progress_panel(drawer, "Updating NX Gallery", "Preparing download...",
                                  "Please wait", update_current_.load(),
                                  update_total_.load(), rise);
            return;
        }
        dialog_face(drawer, 170, kDialogX, kDialogY + rise, kDialogWidth,
                    kDialogHeight, kDialogButtonY + rise);
        text_center(drawer, "NX Gallery update", 29, kInk,
                    kWidth / 2, 252 + rise);
        text_center(drawer, clipped(update_notice_, 62), 20, kInk,
                    kWidth / 2, 322 + rise);
        text_center(drawer, "OK", 24, kDialogAction,
                    kWidth / 2, kDialogButtonY + 20 + rise);
    }

    GalleryController &controller_;
    std::string &status_;
    VideoPlayer &video_player_;
    std::atomic<std::uint64_t> &transfer_current_;
    std::atomic<std::uint64_t> &transfer_total_;
    std::atomic<bool> &transfer_cancel_requested_;
    bool &setup_active_;
    std::string &setup_url_;
    std::string &setup_notice_;
    bool &setup_fullscreen_;
    bool &album_loading_;
    bool &update_notice_active_;
    std::string &update_notice_;
    bool &update_available_;
    bool &update_installing_;
    std::atomic<std::uint64_t> &update_current_;
    std::atomic<std::uint64_t> &update_total_;
    std::vector<TextSlot> text_slots_;
    std::vector<ImageSlot> image_slots_;
    std::vector<ThumbnailSlot> thumbnail_slots_;
    std::vector<ThumbnailState> thumbnail_states_;
    std::vector<HintZone> hint_zones_;
    std::string qr_key_;
    std::vector<std::uint8_t> qr_modules_;
    int qr_size_{};
    std::size_t text_slot_{};
    std::uint8_t render_alpha_{255};
    std::uint32_t pulse_frame_{};
    bool thumbnail_loading_started_{};
    Screen shown_screen_{Screen::Grid};
    Screen transition_from_{Screen::Grid};
    std::size_t shown_media_index_{};
    std::size_t previous_media_index_{};
    std::int32_t navigation_direction_{1};
    std::uint32_t transition_frame_{kTransitionFrames};
    double transition_linear_t_{1.0};
    double transition_t_{1.0};
    bool viewer_navigation_transition_{};
    bool shown_update_notice_{};
};

GalleryApplication::GalleryApplication(pu::ui::render::Renderer::Ref renderer,
                                       AlbumScanResult album,
                                       std::unique_ptr<TelegramBot> bot,
                                       std::string telegram_status,
                                       bool telegram_token_present,
                                       bool release_updates_enabled,
                                       std::string installed_nro_path)
    : Application(std::move(renderer)), bot_(std::move(bot)),
      status_(std::move(telegram_status)),
      telegram_token_present_(telegram_token_present),
      release_updates_enabled_(release_updates_enabled),
      installed_nro_path_(std::move(installed_nro_path)) {
    controller_.set_media(std::move(album.items));
    if (!album.error.empty()) status_ = album.error;
    else if (album.truncated) status_ = "Album limited to the newest 5000 captures";
}

GalleryApplication::~GalleryApplication() {
    if (setup_server_) setup_server_->stop();
    update_cancel_requested_ = true;
    if (share_worker_.joinable()) share_worker_.join();
    if (chat_refresh_worker_.joinable()) chat_refresh_worker_.join();
    if (album_worker_.joinable()) album_worker_.join();
    if (update_worker_.joinable()) update_worker_.join();
}

void GalleryApplication::OnLoad() {
    video_player_ = std::make_unique<VideoPlayer>(pu::ui::render::GetMainRenderer());
    layout_ = pu::ui::Layout::New();
    layout_->SetBackgroundColor(kBackground);
    element_ = std::make_shared<GalleryElement>(controller_, status_, *video_player_,
                                                transfer_current_, transfer_total_,
                                                transfer_cancel_requested_,
                                                setup_active_, setup_url_,
                                                setup_notice_, setup_fullscreen_,
                                                album_loading_,
                                                update_notice_active_, update_notice_,
                                                update_available_, update_installing_,
                                                update_current_, update_total_);
    layout_->Add(element_);
    LoadLayout(layout_);
    SetOnInput([this](const u64 down, const u64, const u64 held, const pu::ui::TouchPoint touch) {
        on_input(down, held, touch);
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
        poll_token_setup();
        poll_album_scan();
        poll_release_update();
        advance_automation();
    });
    if (bot_) {
        std::vector<TelegramChat> cached;
        bot_->cached_chats(cached);
        controller_.set_chats(std::move(cached));
        start_chat_refresh();
    } else if (!telegram_token_present_) {
        open_token_setup(true);
    }
    start_album_scan();
    start_release_check();
}

void GalleryApplication::start_release_check() {
#ifdef NXGALLERY_AUTOMATION_BUILD
    struct stat marker {};
    if (stat(kAutomationUpdateMarker, &marker) != 0 ||
        !S_ISREG(marker.st_mode)) return;
#endif
    if (!release_updates_enabled_ || update_worker_.joinable()) return;
    update_worker_ = std::thread([this] {
        UpdateResult result = check_latest_release(
            NXGALLERY_VERSION, &update_cancel_requested_);
        std::lock_guard<std::mutex> lock(update_mutex_);
        update_result_ = std::move(result);
    });
}

void GalleryApplication::start_release_install() {
    if (!available_update_ || update_worker_.joinable() || update_installing_) return;
    update_installing_ = true;
    update_current_ = 0;
    update_total_ = available_update_->asset_size;
    update_notice_ = "Downloading and verifying " + available_update_->version + "...";
    update_notice_active_ = true;
    const UpdateResult release = *available_update_;
    update_worker_ = std::thread([this, release] {
        auto progress = [this](std::uint64_t current, std::uint64_t total) {
            update_current_ = current;
            if (total > 0) update_total_ = total;
            return !update_cancel_requested_.load();
        };
        UpdateResult result = install_release(
            release, installed_nro_path_, &update_cancel_requested_, progress);
        std::lock_guard<std::mutex> lock(update_mutex_);
        update_result_ = std::move(result);
    });
}

void GalleryApplication::poll_release_update() {
    std::optional<UpdateResult> result;
    {
        std::lock_guard<std::mutex> lock(update_mutex_);
        result.swap(update_result_);
    }
    if (!result) return;
    if (update_worker_.joinable()) update_worker_.join();
    const char *outcome = result->outcome == UpdateOutcome::Available ? "available" :
        result->outcome == UpdateOutcome::Installed ? "installed" :
        result->outcome == UpdateOutcome::Failed ? "failed" : "current";
    std::printf("NXGALLERY_DIAGNOSTIC event=release_update outcome=%s version=%s message=%s\n",
                outcome, result->version.c_str(), result->message.c_str());
    if (result->outcome == UpdateOutcome::Available) {
        available_update_ = std::move(*result);
        update_available_ = true;
        return;
    }
    if (update_installing_) {
        update_installing_ = false;
        if (result->outcome == UpdateOutcome::Installed) {
            update_available_ = false;
            available_update_.reset();
        }
        update_notice_ = std::move(result->message);
        update_notice_active_ = true;
    }
}

void GalleryApplication::start_album_scan() {
    if (album_worker_.joinable() || !controller_.media().empty()) return;
    album_loading_ = true;
    if (controller_.media().empty() && status_.empty()) status_ = "Loading captures...";
    album_worker_ = std::thread([this] {
#ifdef NXGALLERY_AUTOMATION_BUILD
        AlbumScanResult album = scan_album(kRawAlbumPath);
#else
        AlbumScanResult album = scan_horizon_album();
        if (!album || album.items.empty()) {
            AlbumScanResult raw_album = scan_album(kRawAlbumPath);
            if (raw_album && !raw_album.items.empty()) album = std::move(raw_album);
        }
#endif
        std::lock_guard<std::mutex> lock(album_mutex_);
        album_result_ = std::move(album);
    });
}

void GalleryApplication::poll_album_scan() {
    std::optional<AlbumScanResult> album;
    {
        std::lock_guard<std::mutex> lock(album_mutex_);
        album.swap(album_result_);
    }
    if (!album) return;
    if (album_worker_.joinable()) album_worker_.join();
    album_loading_ = false;
    controller_.set_media(std::move(album->items));
    if (!album->error.empty()) status_ = std::move(album->error);
    else if (album->truncated) status_ = "Album limited to the newest 5000 captures";
    else if (status_ == "Loading captures...") status_.clear();
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
    } else if ((automation_frame_ == 360 || automation_frame_ == 380 ||
                automation_frame_ == 400) &&
               controller_.screen() == Screen::Viewer) {
        video_player_->stop();
        controller_.handle(automation_frame_ == 400 ? Action::Right : Action::Left);
        const auto &media = controller_.media();
        if (!media.empty() &&
            media[controller_.selected_media_index()].kind == MediaKind::Video) {
            video_player_->play(media[controller_.selected_media_index()]);
        }
    } else if (automation_frame_ > 420 && controller_.screen() == Screen::Viewer) {
        std::FILE *trigger = std::fopen(kSendTrigger, "rb");
        if (trigger == nullptr) return;
        std::fclose(trigger);
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
    if (!bot_ || share_worker_.joinable() || request.media.empty()) {
        controller_.finish_share(false, "Telegram is unavailable");
        return;
    }
    transfer_current_ = 0;
    std::uint64_t expected_total = 0;
    for (const MediaItem &item : request.media) expected_total += item.size;
    transfer_total_ = expected_total;
    transfer_cancel_requested_ = false;
    share_worker_ = std::thread([this, request = std::move(request)]() mutable {
        auto progress =
            [this](std::uint64_t current, std::uint64_t total) {
                transfer_current_ = current;
                if (total > 0) transfer_total_ = total;
                return !transfer_cancel_requested_.load();
            };
        BotResult result = request.media.size() > 1
            ? bot_->send_media_group(request.media, request.chat, progress)
            : bot_->send_media(request.media.front(), request.chat, progress);
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

void GalleryApplication::toggle_video_playback() {
    const auto &media = controller_.media();
    if (media.empty() ||
        media[controller_.selected_media_index()].kind != MediaKind::Video) {
        return;
    }
    if (video_player_->active()) video_player_->toggle_pause();
    else video_player_->play(media[controller_.selected_media_index()]);
}

void GalleryApplication::on_swipe(std::int32_t dx, std::int32_t dy) {
    if (setup_active_) return;
    const bool vertical = std::abs(dy) >= std::abs(dx);
    if (controller_.screen() == Screen::Grid && vertical) {
        const auto &media = controller_.media();
        if (media.empty()) return;
        constexpr std::size_t page =
            GalleryController::kGridColumns * GalleryController::kVisibleRows;
        const std::size_t index = controller_.selected_media_index();
        if (dy < 0) controller_.select_media(std::min(index + page, media.size() - 1));
        else controller_.select_media(index >= page ? index - page : 0);
        return;
    }
    if (controller_.screen() == Screen::Viewer && !vertical) {
        if (video_player_) video_player_->stop();
        controller_.handle(dx < 0 ? Action::Right : Action::Left);
        return;
    }
    if (controller_.screen() == Screen::ChatPicker && vertical) {
        const auto &chats = controller_.chats();
        if (chats.empty()) return;
        const std::size_t step = 6;
        const std::size_t index = controller_.selected_chat_index();
        if (dy < 0) controller_.select_chat(std::min(index + step, chats.size() - 1));
        else controller_.select_chat(index >= step ? index - step : 0);
    }
}

bool GalleryApplication::dispatch_hint(pu::ui::TouchPoint touch) {
    const auto tag = element_->hint_at(static_cast<std::int32_t>(touch.x),
                                       static_cast<std::int32_t>(touch.y));
    if (!tag) return false;
    std::printf("NXGALLERY_DIAGNOSTIC event=touch_hint tag=%d\n",
                static_cast<int>(*tag));
    switch (*tag) {
        case HintTag::View: controller_.handle(Action::Confirm); break;
        case HintTag::PlayPause: toggle_video_playback(); break;
        case HintTag::Prev:
        case HintTag::Next:
            if (video_player_) video_player_->stop();
            controller_.handle(*tag == HintTag::Prev ? Action::Left : Action::Right);
            break;
        case HintTag::Share: open_chat_picker(); break;
        case HintTag::MultiSelect:
            controller_.handle(Action::ToggleMultiSelect);
            break;
        case HintTag::Update: start_release_install(); break;
        case HintTag::Back:
            if (video_player_) video_player_->stop();
            controller_.handle(Action::Back);
            break;
    }
    return true;
}

void GalleryApplication::on_touch(pu::ui::TouchPoint touch) {
    if (update_notice_active_) {
        if (update_installing_) return;
        update_notice_active_ = false;
        return;
    }
    if (setup_active_) {
        if (touch.HitsRegion(kPickerX, kPickerButtonY, kPickerWidth,
                             kPickerButtonHeight)) {
            close_token_setup();
            status_ = "Telegram setup cancelled";
        }
        return;
    }
    if ((controller_.screen() == Screen::Grid ||
         controller_.screen() == Screen::Viewer) && dispatch_hint(touch)) {
        return;
    }
    if (controller_.screen() == Screen::Grid) {
        const std::size_t start = controller_.grid_page_start();
        for (std::size_t visible = 0;
             visible < GalleryController::kGridColumns * GalleryController::kVisibleRows;
             ++visible) {
            const std::size_t index = start + visible;
            if (index >= controller_.media().size()) break;
            const std::int32_t column = static_cast<std::int32_t>(visible % GalleryController::kGridColumns);
            const std::int32_t row = static_cast<std::int32_t>(visible / GalleryController::kGridColumns);
            if (touch.HitsRegion(kGridX + column * kCellStrideX,
                                 kGridY + row * kCellStrideY, kCellWidth, kCellHeight)) {
                controller_.select_media(index);
                controller_.handle(Action::Confirm);
                return;
            }
        }
        return;
    }
    if (controller_.screen() == Screen::Viewer) {
        if (touch.HitsRegion(0, 0, kWidth, kHeight - kViewerBarHeight)) {
            toggle_video_playback();
        }
        return;
    }
    if (controller_.screen() == Screen::ChatPicker) {
        const std::int32_t third = kPickerWidth / 3;
        if (touch.HitsRegion(kPickerX, kPickerButtonY, third, kPickerButtonHeight)) {
            controller_.handle(Action::Back);
            return;
        }
        if (touch.HitsRegion(kPickerX + third, kPickerButtonY, third, kPickerButtonHeight)) {
            refresh_chats_from_ui();
            return;
        }
        if (touch.HitsRegion(kPickerX + 2 * third, kPickerButtonY, third, kPickerButtonHeight)) {
            auto request = controller_.handle(Action::Confirm);
            if (request) start_share(std::move(*request));
            return;
        }
        const std::size_t selected = controller_.selected_chat_index();
        const std::size_t start = selected >= 6 ? selected - 5 : 0;
        for (std::size_t row = 0; row < 6 && start + row < controller_.chats().size(); ++row) {
            if (touch.HitsRegion(kPickerRowX, kPickerRowY +
                                 static_cast<std::int32_t>(row) * kPickerRowStride,
                                 kPickerRowWidth, kPickerRowHeight)) {
                controller_.select_chat(start + row);
                return;
            }
        }
        return;
    }
    if (controller_.screen() == Screen::Sending) {
        if (touch.HitsRegion(kDialogX, kDialogButtonY, kDialogWidth, kDialogButtonHeight)) {
            transfer_cancel_requested_ = true;
            status_ = "Cancelling Telegram transfer...";
        }
        return;
    }
    if (controller_.screen() == Screen::Result) controller_.handle(Action::Confirm);
}

void GalleryApplication::open_token_setup(bool fullscreen) {
    if (setup_active_) return;
    if (video_player_) video_player_->stop();
    setup_fullscreen_ = fullscreen;
    if (fullscreen) {
        setup_active_ = true;
        setup_notice_ = "Preparing phone connection...";
    }
    const std::uint32_t host = static_cast<std::uint32_t>(gethostid());
    if (host == 0 || host == INADDR_NONE || host == htonl(INADDR_LOOPBACK)) {
        status_ = "Connect the Switch to a Wi-Fi network first";
        if (fullscreen) setup_notice_ = status_;
        return;
    }
    in_addr address{};
    address.s_addr = host;
    char ip_text[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address, ip_text, sizeof(ip_text)) == nullptr) {
        status_ = "Could not read the console's network address";
        if (fullscreen) setup_notice_ = status_;
        return;
    }
    std::uint32_t raw = 0;
    randomGet(&raw, sizeof(raw));
    char secret[9] = {};
    std::snprintf(secret, sizeof(secret), "%08x", raw);
    auto server = std::make_unique<TokenSetupServer>();
    std::string error;
    if (!server->start(ip_text, kSetupPort, secret, error)) {
        status_ = error;
        if (fullscreen) setup_notice_ = status_;
        return;
    }
    setup_server_ = std::move(server);
    setup_url_ = setup_server_->url();
    setup_notice_ = "Waiting for your phone...";
    setup_active_ = true;
    std::printf("NXGALLERY_DIAGNOSTIC event=token_setup state=open port=%u\n",
                static_cast<unsigned int>(kSetupPort));
}

void GalleryApplication::close_token_setup() {
    setup_active_ = false;
    setup_fullscreen_ = false;
    if (setup_server_) {
        setup_server_->stop();
        setup_server_.reset();
    }
    setup_url_.clear();
    setup_notice_.clear();
}

void GalleryApplication::poll_token_setup() {
    if (setup_server_) {
        const TokenSetupServer::State state = setup_server_->state();
        if (state == TokenSetupServer::State::Received &&
            pending_setup_token_.empty()) {
            pending_setup_token_ = setup_server_->take_token();
            setup_notice_ = "Token received. Connecting to Telegram...";
        } else if (state == TokenSetupServer::State::Failed) {
            status_ = setup_server_->failure_reason();
            close_token_setup();
        }
    }
    // Swapping the bot must wait until no worker thread can still touch it.
    if (!pending_setup_token_.empty() && !share_worker_.joinable() &&
        !chat_refresh_worker_.joinable()) {
        apply_setup_token();
    }
}

void GalleryApplication::apply_setup_token() {
    TelegramConfig config;
    config.bot_token = pending_setup_token_;
    auto existing = load_telegram_config(kTelegramConfigPath);
    if (existing) {
        config.chats = std::move(existing.config->chats);
        config.discover_chats = existing.config->discover_chats;
    }
    const std::string serialized = serialize_telegram_config(config);
    (void)mkdir("sdmc:/switch", 0777);
    (void)mkdir("sdmc:/switch/nxgallery", 0777);
    std::FILE *out = std::fopen(kTelegramConfigPath, "wb");
    const bool persisted = out != nullptr &&
        std::fwrite(serialized.data(), 1, serialized.size(), out) == serialized.size();
    if (out != nullptr) std::fclose(out);
    std::printf("NXGALLERY_DIAGNOSTIC event=telegram_config_persist path=%s result=%s\n",
                kTelegramConfigPath, persisted ? "pass" : "fail");
    bot_ = std::make_unique<TelegramBot>(std::move(config));
    telegram_token_present_ = true;
    std::vector<TelegramChat> cached;
    bot_->cached_chats(cached);
    controller_.set_chats(std::move(cached));
    status_ = persisted ? "Telegram connected. Looking for chats..."
                        : "Token active, but saving to the SD card failed";
    std::fill(pending_setup_token_.begin(), pending_setup_token_.end(), '\0');
    pending_setup_token_.clear();
    close_token_setup();
    start_chat_refresh();
}

void GalleryApplication::refresh_chats_from_ui() {
    if (!bot_) status_ = "Telegram is not configured";
    else if (chat_refresh_worker_.joinable()) status_ = "Chat refresh already running";
    else {
        status_ = "Refreshing chats in background...";
        start_chat_refresh();
    }
}

void GalleryApplication::on_input(std::uint64_t down, std::uint64_t held,
                                  pu::ui::TouchPoint touch) {
    if (!touch.IsEmpty()) {
        // Plutonium scales hid touch coordinates by its constexpr ScreenFactor
        // (1920/1280 = 1.5); our canvas is 1280x720, so undo that here.
        const std::int32_t tx = static_cast<std::int32_t>(touch.x / pu::ui::render::ScreenFactor);
        const std::int32_t ty = static_cast<std::int32_t>(touch.y / pu::ui::render::ScreenFactor);
        if (!touch_down_) {
            touch_down_ = true;
            touch_start_x_ = touch_last_x_ = tx;
            touch_start_y_ = touch_last_y_ = ty;
        } else {
            touch_last_x_ = tx;
            touch_last_y_ = ty;
        }
    } else if (touch_down_) {
        touch_down_ = false;
        const std::int32_t dx = touch_last_x_ - touch_start_x_;
        const std::int32_t dy = touch_last_y_ - touch_start_y_;
        // A press that starts on a hint bar is always a tap: the bars are
        // button rows, and drift along them must not turn into a swipe.
        bool on_hint_bar = false;
        if (controller_.screen() == Screen::Grid) {
            on_hint_bar = touch_start_y_ >= kFooterRuleY;
        } else if (controller_.screen() == Screen::Viewer) {
            on_hint_bar = touch_start_y_ >= kHeight - kViewerBarHeight;
        }
        const bool swipe = !on_hint_bar &&
            (std::abs(dx) >= kSwipeThreshold || std::abs(dy) >= kSwipeThreshold);
        std::printf("NXGALLERY_DIAGNOSTIC event=touch result=%s start=%d,%d delta=%d,%d screen=%d\n",
                    swipe ? "swipe" : "tap", touch_start_x_, touch_start_y_,
                    dx, dy, static_cast<int>(controller_.screen()));
        if (swipe) {
            if (controller_.screen() != Screen::Sending &&
                controller_.screen() != Screen::Result) {
                on_swipe(dx, dy);
            }
        } else {
            on_touch(pu::ui::TouchPoint{static_cast<u32>(touch_start_x_),
                                        static_cast<u32>(touch_start_y_)});
        }
    }

    // Held d-pad or stick directions repeat after a short delay.
    static constexpr std::array<std::uint64_t, 4> kDirectionMasks{
        HidNpadButton_AnyUp, HidNpadButton_AnyDown,
        HidNpadButton_AnyLeft, HidNpadButton_AnyRight};
    constexpr std::uint32_t kRepeatDelayFrames = 18;
    constexpr std::uint32_t kRepeatIntervalFrames = 5;
    std::uint64_t direction_fire = 0;
    for (std::size_t index = 0; index < kDirectionMasks.size(); ++index) {
        const std::uint64_t mask = kDirectionMasks[index];
        if ((down & mask) != 0) {
            dir_hold_frames_[index] = 0;
            direction_fire |= mask;
        } else if ((held & mask) != 0) {
            ++dir_hold_frames_[index];
            if (dir_hold_frames_[index] >= kRepeatDelayFrames &&
                (dir_hold_frames_[index] - kRepeatDelayFrames) % kRepeatIntervalFrames == 0) {
                direction_fire |= mask;
            }
        } else {
            dir_hold_frames_[index] = 0;
        }
    }

    if (setup_active_) {
        if ((down & HidNpadButton_B) != 0) {
            close_token_setup();
            status_ = "Telegram setup cancelled";
        }
        return;
    }
    if (update_notice_active_) {
        if (update_installing_) return;
        if ((down & (HidNpadButton_A | HidNpadButton_B)) != 0) {
            update_notice_active_ = false;
        }
        return;
    }
    if ((down & HidNpadButton_Minus) != 0 &&
        controller_.screen() == Screen::Grid && update_available_) {
        start_release_install();
        return;
    }
    if ((down & HidNpadButton_Plus) != 0 &&
        controller_.screen() == Screen::ChatPicker) {
        open_token_setup();
        return;
    }
    if ((down & HidNpadButton_Plus) != 0 &&
        controller_.screen() == Screen::Grid) {
        controller_.handle(Action::ToggleMultiSelect);
        return;
    }
    if (controller_.screen() == Screen::Sending) {
        if ((down & HidNpadButton_B) != 0) {
            transfer_cancel_requested_ = true;
            status_ = "Cancelling Telegram transfer...";
        }
        return;
    }
    if (controller_.screen() == Screen::Viewer &&
        !controller_.media().empty() &&
        controller_.media()[controller_.selected_media_index()].kind == MediaKind::Video) {
        if ((down & HidNpadButton_A) != 0) {
            toggle_video_playback();
            return;
        }
        if ((down & HidNpadButton_B) != 0 ||
            (direction_fire & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) != 0) {
            video_player_->stop();
        }
    }
    if ((down & HidNpadButton_X) != 0 &&
        (controller_.screen() == Screen::Grid ||
         controller_.screen() == Screen::Viewer)) {
        open_chat_picker();
        return;
    }
    if ((down & HidNpadButton_Y) != 0 && controller_.screen() == Screen::Grid) {
        open_token_setup();
        return;
    }
    if ((down & HidNpadButton_Y) != 0 && controller_.screen() == Screen::ChatPicker) {
        refresh_chats_from_ui();
        return;
    }
    std::optional<ShareRequest> request;
    if ((down & HidNpadButton_A) != 0) request = controller_.handle(Action::Confirm);
    else if ((down & HidNpadButton_B) != 0) controller_.handle(Action::Back);
    else if ((direction_fire & HidNpadButton_AnyLeft) != 0) controller_.handle(Action::Left);
    else if ((direction_fire & HidNpadButton_AnyRight) != 0) controller_.handle(Action::Right);
    else if ((direction_fire & HidNpadButton_AnyUp) != 0) controller_.handle(Action::Up);
    else if ((direction_fire & HidNpadButton_AnyDown) != 0) controller_.handle(Action::Down);
    if (request) start_share(std::move(*request));
}

}  // namespace nxgallery
