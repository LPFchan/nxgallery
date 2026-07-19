#pragma once

#include <nxgallery/model.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nxgallery {

enum class Screen { Grid, Viewer, ChatPicker, Sending, Result };
enum class Action {
    Left, Right, Up, Down, Confirm, Back, Share, Refresh, ToggleMultiSelect
};

struct ShareRequest {
    std::vector<MediaItem> media;
    TelegramChat chat;
};

class GalleryController {
public:
    static constexpr std::size_t kGridColumns = 4;
    static constexpr std::size_t kVisibleRows = 3;
    static constexpr std::size_t kMaximumMultiSelect = 10;

    void set_media(std::vector<MediaItem> media);
    void set_chats(std::vector<TelegramChat> chats);
    void select_media(std::size_t index);
    void select_chat(std::size_t index);
    std::optional<ShareRequest> handle(Action action);
    void finish_share(bool success, std::string message);

    Screen screen() const noexcept { return screen_; }
    const std::vector<MediaItem> &media() const noexcept { return media_; }
    const std::vector<TelegramChat> &chats() const noexcept { return chats_; }
    std::size_t selected_media_index() const noexcept { return media_index_; }
    std::size_t selected_chat_index() const noexcept { return chat_index_; }
    std::size_t grid_page_start() const noexcept;
    bool multi_select_active() const noexcept { return multi_select_active_; }
    Screen share_origin() const noexcept { return share_origin_; }
    bool is_media_selected(std::size_t index) const noexcept;
    std::size_t selected_media_count() const noexcept {
        return selected_media_indices_.size();
    }
    const std::string &result_message() const noexcept { return result_message_; }
    bool share_succeeded() const noexcept { return share_succeeded_; }

private:
    void move_grid(Action action);
    void move_chat(Action action);
    void toggle_current_media_selection();
    std::vector<MediaItem> media_for_share() const;

    std::vector<MediaItem> media_;
    std::vector<TelegramChat> chats_;
    Screen screen_{Screen::Grid};
    std::size_t media_index_{};
    std::size_t chat_index_{};
    std::vector<std::size_t> selected_media_indices_;
    Screen share_origin_{Screen::Viewer};
    std::string result_message_;
    bool share_succeeded_{};
    bool multi_select_active_{};
};

}  // namespace nxgallery
