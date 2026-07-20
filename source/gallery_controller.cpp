#include <nxgallery/gallery_controller.hpp>

#include <algorithm>
#include <utility>

namespace nxgallery {

void GalleryController::set_media(std::vector<MediaItem> media) {
    media_ = std::move(media);
    selected_media_indices_.clear();
    multi_select_active_ = false;
    if (media_.empty()) media_index_ = 0;
    else media_index_ = std::min(media_index_, media_.size() - 1);
}

void GalleryController::set_chats(std::vector<TelegramChat> chats) {
    std::sort(chats.begin(), chats.end(), [](const TelegramChat &left, const TelegramChat &right) {
        if (left.title != right.title) return left.title < right.title;
        return left.id < right.id;
    });
    chats.erase(std::unique(chats.begin(), chats.end(), [](const TelegramChat &left, const TelegramChat &right) {
        return left.id == right.id;
    }), chats.end());
    chats_ = std::move(chats);
    if (chats_.empty()) chat_index_ = 0;
    else chat_index_ = std::min(chat_index_, chats_.size() - 1);
}

void GalleryController::select_media(std::size_t index) {
    if (index < media_.size()) media_index_ = index;
}

void GalleryController::select_chat(std::size_t index) {
    if (index < chats_.size()) chat_index_ = index;
}

std::size_t GalleryController::grid_page_start() const noexcept {
    constexpr std::size_t page_size = kGridColumns * kVisibleRows;
    return page_size == 0 ? 0 : (media_index_ / page_size) * page_size;
}

bool GalleryController::is_media_selected(std::size_t index) const noexcept {
    return std::binary_search(selected_media_indices_.begin(),
                              selected_media_indices_.end(), index);
}

void GalleryController::toggle_current_media_selection() {
    if (media_.empty()) return;
    const auto position = std::lower_bound(selected_media_indices_.begin(),
                                           selected_media_indices_.end(),
                                           media_index_);
    if (position != selected_media_indices_.end() && *position == media_index_) {
        selected_media_indices_.erase(position);
    } else {
        selected_media_indices_.insert(position, media_index_);
    }
}

std::vector<MediaItem> GalleryController::media_for_share() const {
    std::vector<MediaItem> selected;
    if (media_.empty()) return selected;
    if (share_origin_ == Screen::Grid && multi_select_active_) {
        selected.reserve(selected_media_indices_.size());
        for (const std::size_t index : selected_media_indices_) {
            if (index < media_.size()) selected.push_back(media_[index]);
        }
    } else {
        selected.push_back(media_[media_index_]);
    }
    return selected;
}

void GalleryController::move_grid(Action action) {
    if (media_.empty()) return;
    const std::size_t current = media_index_;
    switch (action) {
        case Action::Left: if (current > 0) --media_index_; break;
        case Action::Right: if (current + 1 < media_.size()) ++media_index_; break;
        case Action::Up: if (current >= kGridColumns) media_index_ -= kGridColumns; break;
        case Action::Down: if (current + kGridColumns < media_.size()) media_index_ += kGridColumns; break;
        default: break;
    }
}

void GalleryController::move_chat(Action action) {
    if (chats_.empty()) return;
    if (action == Action::Up && chat_index_ > 0) --chat_index_;
    if (action == Action::Down && chat_index_ + 1 < chats_.size()) ++chat_index_;
}

std::optional<ShareRequest> GalleryController::handle(Action action) {
    if (screen_ == Screen::Grid) {
        move_grid(action);
        if (action == Action::ToggleMultiSelect) {
            multi_select_active_ = !multi_select_active_;
            selected_media_indices_.clear();
        } else if (action == Action::Confirm && !media_.empty()) {
            if (multi_select_active_) toggle_current_media_selection();
            else screen_ = Screen::Viewer;
        } else if (action == Action::Share && !media_.empty() &&
                   (!multi_select_active_ || !selected_media_indices_.empty())) {
            chat_index_ = 0;
            share_origin_ = Screen::Grid;
            screen_ = Screen::ChatPicker;
        }
        return std::nullopt;
    }
    if (screen_ == Screen::Viewer) {
        if (action == Action::Back) screen_ = Screen::Grid;
        else if (action == Action::Left && media_index_ > 0) --media_index_;
        else if (action == Action::Right && media_index_ + 1 < media_.size()) ++media_index_;
        else if (action == Action::Share) {
            chat_index_ = 0;
            share_origin_ = Screen::Viewer;
            screen_ = Screen::ChatPicker;
        }
        return std::nullopt;
    }
    if (screen_ == Screen::ChatPicker) {
        move_chat(action);
        if (action == Action::Back) screen_ = share_origin_;
        else if (action == Action::Confirm && !media_.empty() && !chats_.empty()) {
            std::vector<MediaItem> selected = media_for_share();
            if (selected.empty()) return std::nullopt;
            screen_ = Screen::Sending;
            return ShareRequest{std::move(selected), chats_[chat_index_]};
        }
        return std::nullopt;
    }
    if (screen_ == Screen::Result && (action == Action::Confirm || action == Action::Back)) {
        screen_ = share_origin_;
        if (share_succeeded_ && share_origin_ == Screen::Grid) {
            selected_media_indices_.clear();
            multi_select_active_ = false;
        }
    }
    return std::nullopt;
}

void GalleryController::finish_share(bool success, std::string message) {
    if (screen_ != Screen::Sending) return;
    share_succeeded_ = success;
    result_message_ = std::move(message);
    screen_ = Screen::Result;
}

}  // namespace nxgallery
