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
// Reads only a valid bot_token entry and ignores app-specific sibling fields.
// This lets NX Gallery reuse NX Torrent's credential without coupling the two
// applications' destination and state formats.
std::optional<std::string> load_telegram_bot_token(const char *path) noexcept;
// Renders a config back into the on-disk format. Only file-born ("configured")
// chats are written; discovered chats live in the separate chat cache.
std::string serialize_telegram_config(const TelegramConfig &config);

}  // namespace nxgallery
