#include <nxgallery/telegram_config.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace nxgallery {
namespace {

constexpr std::size_t kMaximumConfigBytes = 32 * 1024;

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
    return value;
}

bool valid_token(std::string_view value) {
    const std::size_t separator = value.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size() || value.size() > 256) return false;
    for (char c : value.substr(0, separator)) if (c < '0' || c > '9') return false;
    for (char c : value.substr(separator + 1)) {
        const bool alphanumeric = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!alphanumeric && c != '_' && c != '-') return false;
    }
    return true;
}

bool parse_id(std::string_view value, std::int64_t &output) {
    if (value.empty()) return false;
    bool negative = value.front() == '-';
    if (negative) value.remove_prefix(1);
    if (value.empty()) return false;
    std::uint64_t parsed = 0;
    const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1U : 0U);
    for (char c : value) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (parsed > (limit - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0) return false;
    if (negative && parsed == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U) output = std::numeric_limits<std::int64_t>::min();
    else output = negative ? -static_cast<std::int64_t>(parsed) : static_cast<std::int64_t>(parsed);
    return true;
}

TelegramConfigResult failure(std::string message, std::size_t line = 0) {
    TelegramConfigResult result;
    result.error = std::move(message);
    result.line = line;
    return result;
}

}  // namespace

TelegramConfigResult parse_telegram_config(std::string_view input) noexcept {
    try {
        if (input.size() > kMaximumConfigBytes) return failure("configuration is too large");
        TelegramConfig config;
        bool token_seen = false;
        bool discover_seen = false;
        std::unordered_set<std::int64_t> chat_ids;
        std::size_t line_number = 0;
        while (!input.empty()) {
            ++line_number;
            const std::size_t newline = input.find('\n');
            std::string_view line = trim(newline == std::string_view::npos ? input : input.substr(0, newline));
            input = newline == std::string_view::npos ? std::string_view{} : input.substr(newline + 1);
            if (line.empty() || line.front() == '#') continue;
            const std::size_t separator = line.find('=');
            if (separator == std::string_view::npos) return failure("malformed configuration line", line_number);
            const std::string_view key = trim(line.substr(0, separator));
            const std::string_view value = trim(line.substr(separator + 1));
            if (key == "bot_token") {
                if (token_seen) return failure("duplicate bot_token", line_number);
                if (!valid_token(value)) return failure("invalid bot_token", line_number);
                config.bot_token.assign(value);
                token_seen = true;
            } else if (key == "discover_chats") {
                if (discover_seen) return failure("duplicate discover_chats", line_number);
                if (value == "true") config.discover_chats = true;
                else if (value != "false") return failure("discover_chats must be true or false", line_number);
                else config.discover_chats = false;
                discover_seen = true;
            } else if (key == "chat") {
                const std::size_t divider = value.find('|');
                std::int64_t id = 0;
                const std::string_view id_text = trim(value.substr(0, divider));
                const std::string_view title = divider == std::string_view::npos ? std::string_view{} : trim(value.substr(divider + 1));
                if (!parse_id(id_text, id) || title.empty() || title.size() > 128) return failure("chat must be ID|Title", line_number);
                if (!chat_ids.insert(id).second) return failure("duplicate chat id", line_number);
                config.chats.push_back({id, std::string(title), "configured"});
            } else {
                return failure("unknown configuration key", line_number);
            }
        }
        if (!token_seen) return failure("bot_token is missing");
        if (config.chats.empty() && !config.discover_chats) return failure("at least one chat is required when discovery is disabled");
        TelegramConfigResult result;
        result.config = std::move(config);
        return result;
    } catch (...) {
        return failure("configuration validation failed");
    }
}

std::string serialize_telegram_config(const TelegramConfig &config) {
    std::string output = "bot_token = " + config.bot_token + "\n";
    output += "discover_chats = ";
    output += config.discover_chats ? "true" : "false";
    output += "\n";
    for (const auto &chat : config.chats) {
        if (chat.type != "configured") continue;
        output += "chat = " + std::to_string(chat.id) + " | " + chat.title + "\n";
    }
    return output;
}

TelegramConfigResult load_telegram_config(const char *path) noexcept {
    try {
        if (path == nullptr || *path == '\0') return failure("configuration path is empty");
        std::ifstream input(path, std::ios::binary);
        if (!input) return failure("configuration could not be read");
        std::string contents;
        std::array<char, 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                if (contents.size() + static_cast<std::size_t>(count) > kMaximumConfigBytes) return failure("configuration is too large");
                contents.append(buffer.data(), static_cast<std::size_t>(count));
            }
        }
        if (!input.eof()) return failure("configuration could not be read");
        return parse_telegram_config(contents);
    } catch (...) {
        return failure("configuration could not be read");
    }
}

std::optional<std::string> load_telegram_bot_token(const char *path) noexcept {
    try {
        if (path == nullptr || *path == '\0') return std::nullopt;
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        std::string contents;
        std::array<char, 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                if (contents.size() + static_cast<std::size_t>(count) > kMaximumConfigBytes) {
                    return std::nullopt;
                }
                contents.append(buffer.data(), static_cast<std::size_t>(count));
            }
        }
        if (!input.eof()) return std::nullopt;

        std::optional<std::string> token;
        std::string_view remaining(contents);
        while (!remaining.empty()) {
            const std::size_t newline = remaining.find('\n');
            std::string_view line = trim(newline == std::string::npos
                ? remaining : remaining.substr(0, newline));
            remaining = newline == std::string::npos
                ? std::string_view{} : remaining.substr(newline + 1);
            if (line.empty() || line.front() == '#') continue;
            const std::size_t separator = line.find('=');
            if (separator == std::string_view::npos ||
                trim(line.substr(0, separator)) != "bot_token") continue;
            const std::string_view value = trim(line.substr(separator + 1));
            if (token || !valid_token(value)) return std::nullopt;
            token = std::string(value);
        }
        return token;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace nxgallery
