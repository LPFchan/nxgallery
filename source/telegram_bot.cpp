#include <nxgallery/telegram_bot.hpp>
#include <nxgallery/https_trust.hpp>

#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace nxgallery {
namespace {

constexpr char kStatePath[] = "/switch/nxgallery/telegram-state.json";
constexpr char kStateTemporaryPath[] = "/switch/nxgallery/telegram-state.tmp";
constexpr std::size_t kMaximumResponseBytes = 1024U * 1024U;
constexpr std::size_t kMaximumStateBytes = 256U * 1024U;
constexpr std::size_t kMaximumChats = 128;
constexpr std::uint64_t kMaximumPhotoBytes = 10U * 1024U * 1024U;
constexpr std::uint64_t kMaximumVideoBytes = 50U * 1024U * 1024U;

using JsonOwner = std::unique_ptr<json_object, decltype(&json_object_put)>;

struct ResponseBody { std::string bytes; bool overflow{}; };

std::size_t append_response(char *data, std::size_t size, std::size_t count, void *context) {
    ResponseBody &body = *static_cast<ResponseBody *>(context);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const std::size_t bytes = size * count;
    if (bytes > kMaximumResponseBytes - body.bytes.size()) { body.overflow = true; return 0; }
    body.bytes.append(data, bytes);
    return bytes;
}

std::string api_url(const TelegramConfig &config, const char *method) {
    return "https://api.telegram.org/bot" + config.bot_token + "/" + method;
}

bool configure_curl(CURL *curl, const TelegramConfig &config, const char *method,
                    ResponseBody &body, std::string &error) {
    if (curl == nullptr) { error = "Could not create Telegram request"; return false; }
    const std::string url = api_url(config, method);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NX-Gallery/0.1");
    curl_easy_setopt(curl, CURLOPT_CAINFO, kHttpsCaFile);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 180000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    return true;
}

JsonOwner parse_json(const std::string &bytes) {
    json_tokener *tokener = json_tokener_new_ex(64);
    if (tokener == nullptr) return JsonOwner(nullptr, json_object_put);
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, bytes.data(), static_cast<int>(bytes.size()));
    const json_tokener_error error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    if (error != json_tokener_success || root == nullptr) {
        if (root != nullptr) json_object_put(root);
        return JsonOwner(nullptr, json_object_put);
    }
    return JsonOwner(root, json_object_put);
}

bool json_string(json_object *object, const char *key, std::string &value, std::size_t maximum = 256) {
    json_object *field = nullptr;
    if (!json_object_object_get_ex(object, key, &field) || field == nullptr || json_object_get_type(field) != json_type_string) return false;
    const char *text = json_object_get_string(field);
    const std::size_t size = json_object_get_string_len(field);
    if (text == nullptr || size == 0 || size > maximum) return false;
    value.assign(text, size);
    return true;
}

bool api_result(const std::string &bytes, JsonOwner &root, json_object *&result, std::string &error) {
    root = parse_json(bytes);
    if (!root || json_object_get_type(root.get()) != json_type_object) { error = "Telegram returned invalid JSON"; return false; }
    json_object *ok = nullptr;
    if (!json_object_object_get_ex(root.get(), "ok", &ok) || json_object_get_type(ok) != json_type_boolean) {
        error = "Telegram returned an invalid response";
        return false;
    }
    if (!json_object_get_boolean(ok)) {
        std::string description;
        error = json_string(root.get(), "description", description) ? description : "Telegram Bot API rejected the request";
        return false;
    }
    if (!json_object_object_get_ex(root.get(), "result", &result) || result == nullptr) {
        error = "Telegram response omitted its result";
        return false;
    }
    return true;
}

bool perform_form(const TelegramConfig &config, const char *method, const std::string &fields,
                  std::string &bytes, std::string &error) {
    CURL *curl = curl_easy_init();
    ResponseBody body;
    if (!configure_curl(curl, config, method, body, error)) return false;
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(fields.size()));
    const CURLcode code = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || body.overflow) {
        error = body.overflow ? "Telegram response exceeded its size limit" : "Telegram HTTPS request failed";
        return false;
    }
    if (response_code < 200 || response_code >= 300) {
        error = "Telegram HTTPS returned status " + std::to_string(response_code);
        return false;
    }
    bytes = std::move(body.bytes);
    return true;
}

std::string escape(CURL *curl, const std::string &value) {
    char *encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (encoded == nullptr) return {};
    std::string result(encoded);
    curl_free(encoded);
    return result;
}

std::string chat_title(json_object *chat, std::int64_t id) {
    std::string value;
    if (json_string(chat, "title", value, 128)) return value;
    std::string first;
    std::string last;
    (void)json_string(chat, "first_name", first, 64);
    (void)json_string(chat, "last_name", last, 64);
    if (!first.empty() || !last.empty()) return first + (first.empty() || last.empty() ? "" : " ") + last;
    if (json_string(chat, "username", value, 64)) return "@" + value;
    return "Chat " + std::to_string(id);
}

bool parse_chat(json_object *chat, TelegramChat &result) {
    if (chat == nullptr || json_object_get_type(chat) != json_type_object) return false;
    json_object *id = nullptr;
    if (!json_object_object_get_ex(chat, "id", &id) || json_object_get_type(id) != json_type_int) return false;
    result.id = json_object_get_int64(id);
    if (result.id == 0) return false;
    result.title = chat_title(chat, result.id);
    if (!json_string(chat, "type", result.type, 32)) result.type = "unknown";
    return true;
}

void merge_chat(std::vector<TelegramChat> &chats, TelegramChat chat) {
    auto found = std::find_if(chats.begin(), chats.end(), [&chat](const TelegramChat &entry) { return entry.id == chat.id; });
    if (found == chats.end()) {
        if (chats.size() < kMaximumChats) chats.push_back(std::move(chat));
    } else if (!chat.title.empty()) {
        *found = std::move(chat);
    }
}

bool read_state(std::int64_t &offset, std::vector<TelegramChat> &chats) {
    std::ifstream input(kStatePath, std::ios::binary);
    if (!input) return false;
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() > kMaximumStateBytes) return false;
    JsonOwner root = parse_json(bytes);
    if (!root || json_object_get_type(root.get()) != json_type_object) return false;
    json_object *offset_object = nullptr;
    if (json_object_object_get_ex(root.get(), "next_update_offset", &offset_object) && json_object_get_type(offset_object) == json_type_int) {
        offset = std::max<std::int64_t>(0, json_object_get_int64(offset_object));
    }
    json_object *array = nullptr;
    if (!json_object_object_get_ex(root.get(), "chats", &array) || json_object_get_type(array) != json_type_array) return true;
    const std::size_t count = std::min<std::size_t>(json_object_array_length(array), kMaximumChats);
    for (std::size_t index = 0; index < count; ++index) {
        TelegramChat chat;
        if (parse_chat(json_object_array_get_idx(array, index), chat)) merge_chat(chats, std::move(chat));
    }
    return true;
}

bool save_state(std::int64_t offset, const std::vector<TelegramChat> &chats) {
    JsonOwner root(json_object_new_object(), json_object_put);
    if (!root) return false;
    json_object_object_add(root.get(), "next_update_offset", json_object_new_int64(offset));
    json_object *array = json_object_new_array();
    if (array == nullptr) return false;
    json_object_object_add(root.get(), "chats", array);
    for (const TelegramChat &chat : chats) {
        json_object *entry = json_object_new_object();
        if (entry == nullptr) return false;
        json_object_object_add(entry, "id", json_object_new_int64(chat.id));
        json_object_object_add(entry, "title", json_object_new_string_len(chat.title.data(), static_cast<int>(chat.title.size())));
        json_object_object_add(entry, "type", json_object_new_string_len(chat.type.data(), static_cast<int>(chat.type.size())));
        json_object_array_add(array, entry);
    }
    const char *serialized = json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
    if (serialized == nullptr) return false;
    std::ofstream output(kStateTemporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << serialized << '\n';
    output.close();
    if (!output) { std::remove(kStateTemporaryPath); return false; }
    if (std::rename(kStateTemporaryPath, kStatePath) != 0) { std::remove(kStateTemporaryPath); return false; }
    return true;
}

}  // namespace

TelegramBot::TelegramBot(TelegramConfig config) : config_(std::move(config)), cached_chats_(config_.chats) {
    (void)read_state(next_update_offset_, cached_chats_);
}

BotResult TelegramBot::refresh_chats(std::vector<TelegramChat> &chats) noexcept {
    try {
        cached_chats_.insert(cached_chats_.end(), config_.chats.begin(), config_.chats.end());
        std::vector<TelegramChat> deduplicated;
        for (const auto &chat : cached_chats_) merge_chat(deduplicated, chat);
        cached_chats_ = std::move(deduplicated);
        if (!config_.discover_chats) {
            chats = cached_chats_;
            return {true, "Configured chats loaded"};
        }
        CURL *encoder = curl_easy_init();
        if (encoder == nullptr) { chats = cached_chats_; return {!chats.empty(), "Could not prepare chat discovery"}; }
        const std::string updates = "limit=100&timeout=0&offset=" + std::to_string(next_update_offset_) +
            "&allowed_updates=" + escape(encoder, "[\"message\",\"edited_message\",\"channel_post\",\"edited_channel_post\",\"my_chat_member\"]");
        curl_easy_cleanup(encoder);
        std::string bytes;
        std::string error;
        if (!perform_form(config_, "getUpdates", updates, bytes, error)) {
            chats = cached_chats_;
            return {!chats.empty(), chats.empty() ? error : "Using cached chats; " + error};
        }
        JsonOwner root(nullptr, json_object_put);
        json_object *result = nullptr;
        if (!api_result(bytes, root, result, error) || json_object_get_type(result) != json_type_array) {
            chats = cached_chats_;
            return {!chats.empty(), chats.empty() ? error : "Using cached chats; " + error};
        }
        const std::size_t count = json_object_array_length(result);
        for (std::size_t index = 0; index < count; ++index) {
            json_object *update = json_object_array_get_idx(result, index);
            if (update == nullptr || json_object_get_type(update) != json_type_object) continue;
            json_object *update_id = nullptr;
            if (json_object_object_get_ex(update, "update_id", &update_id) && json_object_get_type(update_id) == json_type_int) {
                const std::int64_t id = json_object_get_int64(update_id);
                if (id >= 0 && id < std::numeric_limits<std::int64_t>::max()) next_update_offset_ = std::max(next_update_offset_, id + 1);
            }
            json_object *container = nullptr;
            static constexpr std::array<const char *, 5> keys{{"message", "edited_message", "channel_post", "edited_channel_post", "my_chat_member"}};
            for (const char *key : keys) if (json_object_object_get_ex(update, key, &container)) break;
            if (container == nullptr || json_object_get_type(container) != json_type_object) continue;
            json_object *chat_object = nullptr;
            if (!json_object_object_get_ex(container, "chat", &chat_object)) continue;
            TelegramChat chat;
            if (parse_chat(chat_object, chat)) merge_chat(cached_chats_, std::move(chat));
        }
        (void)save_state(next_update_offset_, cached_chats_);
        chats = cached_chats_;
        return {true, count == 0 ? "No new chats; showing saved destinations" : "Chat destinations refreshed"};
    } catch (...) {
        chats = cached_chats_;
        return {!chats.empty(), chats.empty() ? "Chat discovery failed" : "Using cached chats"};
    }
}

BotResult TelegramBot::send_media(const MediaItem &media, const TelegramChat &chat) noexcept {
    try {
        struct stat status {};
        if (chat.id == 0 || stat(media.path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) return {false, "Selected capture is unavailable"};
        const std::uint64_t size = status.st_size < 0 ? 0U : static_cast<std::uint64_t>(status.st_size);
        const std::uint64_t limit = media.kind == MediaKind::Photo ? kMaximumPhotoBytes : kMaximumVideoBytes;
        if (size == 0 || size > limit) return {false, media.kind == MediaKind::Photo ? "Photo exceeds the Bot API 10 MB limit" : "Video exceeds the Bot API 50 MB limit"};
        CURL *curl = curl_easy_init();
        ResponseBody body;
        std::string error;
        const char *method = media.kind == MediaKind::Photo ? "sendPhoto" : "sendVideo";
        if (!configure_curl(curl, config_, method, body, error)) return {false, error};
        curl_mime *mime = curl_mime_init(curl);
        if (mime == nullptr) { curl_easy_cleanup(curl); return {false, "Could not prepare media upload"}; }
        curl_mimepart *part = curl_mime_addpart(mime);
        curl_mime_name(part, "chat_id");
        const std::string chat_id = std::to_string(chat.id);
        curl_mime_data(part, chat_id.c_str(), CURL_ZERO_TERMINATED);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, media.kind == MediaKind::Photo ? "photo" : "video");
        curl_mime_filedata(part, media.path.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        const CURLcode code = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        if (code != CURLE_OK || body.overflow) return {false, body.overflow ? "Telegram response exceeded its size limit" : "Telegram upload failed"};
        JsonOwner root(nullptr, json_object_put);
        json_object *result = nullptr;
        if (!api_result(body.bytes, root, result, error)) {
            return {false, error.empty() ? "Telegram rejected the upload" : error};
        }
        if (response_code < 200 || response_code >= 300) {
            return {false, "Telegram HTTPS returned status " + std::to_string(response_code)};
        }
        return {true, "Sent to " + chat.title};
    } catch (...) {
        return {false, "Telegram upload failed"};
    }
}

}  // namespace nxgallery
