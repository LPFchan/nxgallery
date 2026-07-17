#pragma once

#include <nxgallery/model.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nxgallery {

enum class Screen { Grid, Viewer, ChatPicker, Sending, Result };
enum class Action { Left, Right, Up, Down, Confirm, Back, Share, Refresh };

struct ShareRequest {
    MediaItem media;
    TelegramChat chat;
};

class GalleryController {
public:
    static constexpr std::size_t kGridColumns = 4;
    static constexpr std::size_t kVisibleRows = 3;

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
    const std::string &result_message() const noexcept { return result_message_; }
    bool share_succeeded() const noexcept { return share_succeeded_; }

private:
    void move_grid(Action action);
    void move_chat(Action action);

    std::vector<MediaItem> media_;
    std::vector<TelegramChat> chats_;
    Screen screen_{Screen::Grid};
    std::size_t media_index_{};
    std::size_t chat_index_{};
    std::string result_message_;
    bool share_succeeded_{};
};

}  // namespace nxgallery
