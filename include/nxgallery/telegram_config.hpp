#pragma once

#include <nxgallery/model.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxgallery {

struct TelegramConfig {
    std::string bot_token;
    std::vector<TelegramChat> chats;
    bool discover_chats{true};
};

struct TelegramConfigResult {
    std::optional<TelegramConfig> config;
    std::string error;
    std::size_t line{};
    explicit operator bool() const noexcept { return config.has_value(); }
};

TelegramConfigResult parse_telegram_config(std::string_view input) noexcept;
TelegramConfigResult load_telegram_config(const char *path) noexcept;

}  // namespace nxgallery
