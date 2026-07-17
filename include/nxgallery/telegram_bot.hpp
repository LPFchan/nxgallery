#pragma once

#include <nxgallery/model.hpp>
#include <nxgallery/telegram_config.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nxgallery {

struct BotResult {
    bool success{};
    std::string message;
};

class TelegramBot {
public:
    explicit TelegramBot(TelegramConfig config);

    BotResult refresh_chats(std::vector<TelegramChat> &chats) noexcept;
    BotResult send_media(const MediaItem &media, const TelegramChat &chat) noexcept;

private:
    TelegramConfig config_;
    std::int64_t next_update_offset_{};
    std::vector<TelegramChat> cached_chats_;
};

}  // namespace nxgallery
