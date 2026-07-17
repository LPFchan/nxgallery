#pragma once

#include <nxgallery/model.hpp>
#include <nxgallery/telegram_config.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nxgallery {

struct BotResult {
    bool success{};
    std::string message;
};

class TelegramBot {
public:
    using TransferProgress = std::function<void(std::uint64_t, std::uint64_t)>;

    explicit TelegramBot(TelegramConfig config);

    void cached_chats(std::vector<TelegramChat> &chats) const;
    BotResult refresh_chats(std::vector<TelegramChat> &chats) noexcept;
    BotResult send_media(const MediaItem &media, const TelegramChat &chat,
                         TransferProgress progress = {}) noexcept;

private:
    TelegramConfig config_;
    std::int64_t next_update_offset_{};
    std::vector<TelegramChat> cached_chats_;
};

}  // namespace nxgallery
