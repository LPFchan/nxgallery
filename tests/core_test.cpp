#include <nxgallery/album_index.hpp>
#include <nxgallery/gallery_controller.hpp>
#include <nxgallery/release_update.hpp>
#include <nxgallery/telegram_batches.hpp>
#include <nxgallery/telegram_config.hpp>
#include <nxgallery/token_setup.hpp>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

namespace {

void controller_flow() {
    nxgallery::GalleryController controller;
    controller.set_media({
        {"a.jpg", "a.jpg", nxgallery::MediaKind::Photo, 2, 10},
        {"b.mp4", "b.mp4", nxgallery::MediaKind::Video, 1, 20},
    });
    controller.set_chats({{2, "Zeta", "group"}, {1, "Alpha", "private"}});
    controller.select_media(1);
    assert(controller.selected_media_index() == 1);
    controller.select_media(99);
    assert(controller.selected_media_index() == 1);
    controller.select_chat(1);
    assert(controller.selected_chat_index() == 1);
    controller.select_chat(99);
    assert(controller.selected_chat_index() == 1);
    controller.select_chat(0);
    assert(controller.screen() == nxgallery::Screen::Grid);
    controller.handle(nxgallery::Action::Confirm);
    assert(controller.screen() == nxgallery::Screen::Viewer);
    controller.handle(nxgallery::Action::Share);
    assert(controller.screen() == nxgallery::Screen::ChatPicker);
    assert(controller.chats().front().title == "Alpha");
    const auto request = controller.handle(nxgallery::Action::Confirm);
    assert(request.has_value());
    assert(request->media.size() == 1);
    assert(request->media.front().kind == nxgallery::MediaKind::Video);
    assert(request->media.front().filename == "b.mp4");
    assert(request->chat.id == 1);
    assert(controller.screen() == nxgallery::Screen::Sending);
    controller.finish_share(true, "Sent");
    assert(controller.screen() == nxgallery::Screen::Result);
    assert(controller.share_succeeded());
    controller.handle(nxgallery::Action::Back);
    assert(controller.screen() == nxgallery::Screen::Viewer);
}

void grid_multi_select_flow() {
    nxgallery::GalleryController controller;
    std::vector<nxgallery::MediaItem> media;
    for (int index = 0; index < 25; ++index) {
        media.push_back({std::to_string(index) + ".jpg", std::to_string(index),
                         nxgallery::MediaKind::Photo, 25 - index, 1});
    }
    controller.set_media(std::move(media));
    controller.set_chats({{42, "Saved", "private"}});

    controller.handle(nxgallery::Action::Share);
    assert(controller.screen() == nxgallery::Screen::ChatPicker);
    auto request = controller.handle(nxgallery::Action::Confirm);
    assert(request && request->media.size() == 1);
    assert(request->media.front().filename == "0");
    controller.finish_share(true, "Sent");
    controller.handle(nxgallery::Action::Confirm);
    assert(controller.screen() == nxgallery::Screen::Grid);

    controller.handle(nxgallery::Action::ToggleMultiSelect);
    assert(controller.multi_select_active());
    for (int index = 0; index < 25; ++index) {
        controller.select_media(static_cast<std::size_t>(index));
        controller.handle(nxgallery::Action::Confirm);
    }
    assert(controller.selected_media_count() == 25);
    assert(controller.is_media_selected(0));
    assert(controller.is_media_selected(24));
    controller.select_media(0);
    controller.handle(nxgallery::Action::Confirm);
    assert(!controller.is_media_selected(0));
    assert(controller.selected_media_count() == 24);

    controller.handle(nxgallery::Action::Share);
    assert(controller.screen() == nxgallery::Screen::ChatPicker);
    controller.handle(nxgallery::Action::Back);
    assert(controller.screen() == nxgallery::Screen::Grid);
    assert(controller.multi_select_active());
    assert(controller.selected_media_count() == 24);
    controller.handle(nxgallery::Action::Share);
    request = controller.handle(nxgallery::Action::Confirm);
    assert(request && request->media.size() == 24);
    for (std::size_t index = 0; index < request->media.size(); ++index) {
        assert(request->media[index].filename == std::to_string(24 - index));
    }
    controller.finish_share(false, "Transfer cancelled");
    controller.handle(nxgallery::Action::Confirm);
    assert(controller.screen() == nxgallery::Screen::Grid);
    assert(controller.multi_select_active());
    assert(controller.selected_media_count() == 24);
    controller.handle(nxgallery::Action::Share);
    request = controller.handle(nxgallery::Action::Confirm);
    assert(request && request->media.size() == 24);
    controller.finish_share(true, "Sent album");
    controller.handle(nxgallery::Action::Confirm);
    assert(controller.screen() == nxgallery::Screen::Grid);
    assert(!controller.multi_select_active());
    assert(controller.selected_media_count() == 0);
}

std::vector<nxgallery::MediaItem> batching_media(std::size_t count) {
    std::vector<nxgallery::MediaItem> media;
    media.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        media.push_back({std::to_string(index) + ".jpg", std::to_string(index),
                         nxgallery::MediaKind::Photo,
                         static_cast<std::int64_t>(index), index + 1});
    }
    return media;
}

void telegram_batching_flow() {
    const auto media = batching_media(25);
    std::vector<std::vector<std::string>> calls;
    std::vector<std::uint64_t> progress_values;
    std::vector<std::uint64_t> progress_totals;

    const auto result = nxgallery::send_telegram_batches(
        media,
        [&](std::uint64_t current, std::uint64_t total) {
            progress_values.push_back(current);
            progress_totals.push_back(total);
            return true;
        },
        [&](const nxgallery::MediaItem &item,
            nxgallery::TelegramBot::TransferProgress progress) {
            calls.push_back({item.filename});
            assert(progress(item.size / 2, item.size));
            assert(progress(item.size, item.size));
            return nxgallery::BotResult{true, "sent"};
        },
        [&](const std::vector<nxgallery::MediaItem> &batch,
            nxgallery::TelegramBot::TransferProgress progress) {
            std::vector<std::string> names;
            std::uint64_t bytes = 0;
            for (const auto &item : batch) {
                names.push_back(item.filename);
                bytes += item.size;
            }
            calls.push_back(std::move(names));
            assert(progress(bytes / 2, bytes));
            assert(progress(bytes, bytes));
            return nxgallery::BotResult{true, "sent"};
        });

    assert(result.success);
    assert(calls.size() == 3);
    assert(calls[0].size() == 10);
    assert(calls[1].size() == 10);
    assert(calls[2].size() == 5);
    assert(calls[0].front() == "0" && calls[0].back() == "9");
    assert(calls[1].front() == "10" && calls[1].back() == "19");
    assert(calls[2].front() == "20" && calls[2].back() == "24");
    assert(progress_values.size() == progress_totals.size());
    assert(!progress_values.empty());
    for (std::size_t index = 1; index < progress_values.size(); ++index) {
        assert(progress_values[index] >= progress_values[index - 1]);
        assert(progress_totals[index] == progress_totals[0]);
    }
    assert(progress_values.back() == progress_totals.back());
}

void telegram_batching_stops_on_failure() {
    const auto media = batching_media(25);
    int group_calls = 0;
    int single_calls = 0;
    const auto result = nxgallery::send_telegram_batches(
        media, {},
        [&](const nxgallery::MediaItem &,
            nxgallery::TelegramBot::TransferProgress) {
            ++single_calls;
            return nxgallery::BotResult{true, "sent"};
        },
        [&](const std::vector<nxgallery::MediaItem> &batch,
            nxgallery::TelegramBot::TransferProgress) {
            assert(batch.size() == 10);
            ++group_calls;
            return nxgallery::BotResult{group_calls == 1,
                                         group_calls == 1 ? "sent" : "failed"};
        });
    assert(!result.success);
    assert(result.message == "Some captures were sent; remaining transfer failed");
    assert(group_calls == 2);
    assert(single_calls == 0);
}

void telegram_batching_uses_singleton_remainder() {
    const auto media = batching_media(21);
    int group_calls = 0;
    int single_calls = 0;
    const auto result = nxgallery::send_telegram_batches(
        media, {},
        [&](const nxgallery::MediaItem &item,
            nxgallery::TelegramBot::TransferProgress) {
            assert(item.filename == "20");
            ++single_calls;
            return nxgallery::BotResult{true, "sent"};
        },
        [&](const std::vector<nxgallery::MediaItem> &batch,
            nxgallery::TelegramBot::TransferProgress) {
            assert(batch.size() == 10);
            ++group_calls;
            return nxgallery::BotResult{true, "sent"};
        });
    assert(result.success);
    assert(group_calls == 2);
    assert(single_calls == 1);
}

void telegram_batching_splits_by_request_size() {
    constexpr std::uint64_t mebibyte = 1024U * 1024U;
    std::vector<nxgallery::MediaItem> media{
        {"0.mp4", "0", nxgallery::MediaKind::Video, 0, 20U * mebibyte},
        {"1.mp4", "1", nxgallery::MediaKind::Video, 1, 20U * mebibyte},
        {"2.mp4", "2", nxgallery::MediaKind::Video, 2, 20U * mebibyte},
        {"3.mp4", "3", nxgallery::MediaKind::Video, 3, 50U * mebibyte},
        {"4.jpg", "4", nxgallery::MediaKind::Photo, 4, mebibyte},
        {"5.jpg", "5", nxgallery::MediaKind::Photo, 5, mebibyte},
    };
    std::vector<std::vector<std::string>> calls;
    std::vector<std::uint64_t> progress_values;
    std::vector<std::uint64_t> progress_totals;

    const auto result = nxgallery::send_telegram_batches(
        media,
        [&](std::uint64_t current, std::uint64_t total) {
            progress_values.push_back(current);
            progress_totals.push_back(total);
            return true;
        },
        [&](const nxgallery::MediaItem &item,
            nxgallery::TelegramBot::TransferProgress progress) {
            calls.push_back({item.filename});
            assert(progress(item.size, item.size));
            return nxgallery::BotResult{true, "sent"};
        },
        [&](const std::vector<nxgallery::MediaItem> &batch,
            nxgallery::TelegramBot::TransferProgress progress) {
            std::vector<std::string> names;
            std::uint64_t bytes = 0;
            for (const auto &item : batch) {
                names.push_back(item.filename);
                bytes += item.size;
            }
            calls.push_back(std::move(names));
            assert(bytes <= nxgallery::kMaximumTelegramRequestMediaBytes);
            assert(progress(bytes, bytes));
            return nxgallery::BotResult{true, "sent"};
        });

    assert(result.success);
    assert(calls.size() == 4);
    assert((calls[0] == std::vector<std::string>{"0", "1"}));
    assert((calls[1] == std::vector<std::string>{"2"}));
    assert((calls[2] == std::vector<std::string>{"3"}));
    assert((calls[3] == std::vector<std::string>{"4", "5"}));
    assert(progress_values.size() == progress_totals.size());
    assert(!progress_values.empty());
    for (std::size_t index = 1; index < progress_values.size(); ++index) {
        assert(progress_values[index] >= progress_values[index - 1]);
        assert(progress_totals[index] == progress_totals[0]);
    }
    assert(progress_values.back() == progress_totals.back());
}

void telegram_batching_stops_on_cancellation() {
    const auto media = batching_media(21);
    int group_calls = 0;
    int single_calls = 0;
    std::size_t progress_calls = 0;
    const auto result = nxgallery::send_telegram_batches(
        media,
        [&](std::uint64_t, std::uint64_t) {
            ++progress_calls;
            return progress_calls < 3;
        },
        [&](const nxgallery::MediaItem &,
            nxgallery::TelegramBot::TransferProgress) {
            ++single_calls;
            return nxgallery::BotResult{true, "sent"};
        },
        [&](const std::vector<nxgallery::MediaItem> &batch,
            nxgallery::TelegramBot::TransferProgress progress) {
            ++group_calls;
            std::uint64_t bytes = 0;
            for (const auto &item : batch) bytes += item.size;
            assert(progress(bytes, bytes));
            return nxgallery::BotResult{true, "sent"};
        });
    assert(!result.success);
    assert(result.message ==
           "Some captures were sent before transfer was cancelled");
    assert(group_calls == 1);
    assert(single_calls == 0);
}

void grid_boundaries() {
    nxgallery::GalleryController controller;
    for (int index = 0; index < 14; ++index) {
        auto items = controller.media();
        (void)items;
    }
    std::vector<nxgallery::MediaItem> media;
    for (int index = 0; index < 14; ++index) {
        media.push_back({std::to_string(index) + ".jpg", std::to_string(index), nxgallery::MediaKind::Photo, index, 1});
    }
    controller.set_media(std::move(media));
    controller.handle(nxgallery::Action::Left);
    assert(controller.selected_media_index() == 0);
    for (int index = 0; index < 3; ++index) controller.handle(nxgallery::Action::Down);
    assert(controller.selected_media_index() == 12);
    assert(controller.grid_page_start() == 12);
    controller.handle(nxgallery::Action::Right);
    assert(controller.selected_media_index() == 13);
    controller.handle(nxgallery::Action::Right);
    assert(controller.selected_media_index() == 13);
}

void config_contract() {
    auto parsed = nxgallery::parse_telegram_config(
        "bot_token=123456:abc_DEF-123\n"
        "chat=-1001234567890|Family room\n"
        "chat=42|Me\n");
    assert(parsed);
    assert(parsed.config->discover_chats);
    assert(parsed.config->chats.size() == 2);
    assert(parsed.config->bot_token == "123456:abc_DEF-123");

    parsed = nxgallery::parse_telegram_config(
        "bot_token=123456:abc\n"
        "discover_chats=false\n");
    assert(!parsed);
    assert(parsed.error.find("at least one chat") != std::string::npos);

    parsed = nxgallery::parse_telegram_config(
        "api_id=123\n"
        "bot_token=123456:abc\n");
    assert(!parsed);
    assert(parsed.line == 1);
}

void album_scan_contract() {
    char pattern[] = "/tmp/nxgallery-test.XXXXXX";
    char *root = mkdtemp(pattern);
    assert(root != nullptr);
    const std::string nested = std::string(root) + "/2026/07/17";
    assert(mkdir((std::string(root) + "/2026").c_str(), 0700) == 0);
    assert(mkdir((std::string(root) + "/2026/07").c_str(), 0700) == 0);
    assert(mkdir(nested.c_str(), 0700) == 0);
    { std::ofstream(nested + "/capture.JPG") << "photo"; }
    { std::ofstream(nested + "/clip.mp4") << "video"; }
    { std::ofstream(nested + "/ignore.txt") << "ignore"; }
    const auto result = nxgallery::scan_album(root);
    assert(result);
    assert(result.items.size() == 2);
    bool photo = false;
    bool video = false;
    for (const auto &item : result.items) {
        photo = photo || item.kind == nxgallery::MediaKind::Photo;
        video = video || item.kind == nxgallery::MediaKind::Video;
    }
    assert(photo && video);
    std::remove((nested + "/capture.JPG").c_str());
    std::remove((nested + "/clip.mp4").c_str());
    std::remove((nested + "/ignore.txt").c_str());
    rmdir(nested.c_str());
    rmdir((std::string(root) + "/2026/07").c_str());
    rmdir((std::string(root) + "/2026").c_str());
    rmdir(root);
}

void album_limit_keeps_newest() {
    char pattern[] = "/tmp/nxgallery-limit-test.XXXXXX";
    char *root = mkdtemp(pattern);
    assert(root != nullptr);
    const std::string oldest = std::string(root) + "/a.jpg";
    const std::string newest = std::string(root) + "/b.jpg";
    const std::string middle = std::string(root) + "/c.jpg";
    { std::ofstream(oldest) << "oldest"; }
    { std::ofstream(newest) << "newest"; }
    { std::ofstream(middle) << "middle"; }
    utimbuf timestamp{};
    timestamp.actime = 10;
    timestamp.modtime = 10;
    assert(utime(oldest.c_str(), &timestamp) == 0);
    timestamp.actime = 30;
    timestamp.modtime = 30;
    assert(utime(newest.c_str(), &timestamp) == 0);
    timestamp.actime = 20;
    timestamp.modtime = 20;
    assert(utime(middle.c_str(), &timestamp) == 0);

    const auto result = nxgallery::scan_album(root, 2);
    assert(result);
    assert(result.truncated);
    assert(result.items.size() == 2);
    assert(result.items[0].filename == "b.jpg");
    assert(result.items[1].filename == "c.jpg");

    std::remove(oldest.c_str());
    std::remove(newest.c_str());
    std::remove(middle.c_str());
    rmdir(root);
}

void serialize_contract() {
    nxgallery::TelegramConfig config;
    config.bot_token = "123456:abc_DEF-123";
    config.discover_chats = true;
    config.chats = {{-1001234567890, "Family room", "configured"},
                    {42, "Me", "private"}};
    const std::string serialized = nxgallery::serialize_telegram_config(config);
    const auto parsed = nxgallery::parse_telegram_config(serialized);
    assert(parsed);
    assert(parsed.config->bot_token == config.bot_token);
    assert(parsed.config->discover_chats);
    // Discovered chats belong to the chat cache, not the config file.
    assert(parsed.config->chats.size() == 1);
    assert(parsed.config->chats.front().id == -1001234567890);
    assert(parsed.config->chats.front().title == "Family room");
}

void shared_token_contract() {
    char pattern[] = "/tmp/nxgallery-token-test.XXXXXX";
    const int descriptor = mkstemp(pattern);
    assert(descriptor >= 0);
    close(descriptor);
    {
        std::ofstream output(pattern);
        output << "# NX Torrent configuration\n"
               << "bot_token=123456:shared_DEF-123\n"
               << "allowed_chat_id=-1001234567890\n";
    }
    const auto token = nxgallery::load_telegram_bot_token(pattern);
    assert(token);
    assert(*token == "123456:shared_DEF-123");
    std::remove(pattern);
}

void http_request_contract() {
    nxgallery::HttpRequest request;
    bool malformed = true;
    assert(!nxgallery::parse_http_request("GET /s/ab HTTP/1.1\r\n", request, malformed));
    assert(!malformed);

    assert(nxgallery::parse_http_request(
        "GET /s/abcd1234 HTTP/1.1\r\nHost: 10.0.0.5:8135\r\n\r\n", request, malformed));
    assert(request.method == "GET");
    assert(request.target == "/s/abcd1234");
    assert(request.body.empty());

    const std::string post =
        "POST /s/abcd1234 HTTP/1.1\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "content-length: 23\r\n\r\n"
        "token=123456%3Aabc-DEF_";
    assert(!nxgallery::parse_http_request(post.substr(0, post.size() - 4),
                                          request, malformed));
    assert(!malformed);
    assert(nxgallery::parse_http_request(post, request, malformed));
    assert(request.method == "POST");
    assert(request.body == "token=123456%3Aabc-DEF_");

    assert(!nxgallery::parse_http_request("NOT-HTTP\r\n\r\n", request, malformed));
    assert(malformed);
    assert(!nxgallery::parse_http_request(
        "POST / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", request, malformed));
    assert(malformed);
}

void form_decode_contract() {
    assert(nxgallery::url_decode_form_value("a+b%3Ac%2fd") == "a b:c/d");
    assert(nxgallery::url_decode_form_value("%zz") == "%zz");
    assert(nxgallery::form_field("token=123456%3AabcDEF&other=1", "token") ==
           "123456:abcDEF");
    assert(nxgallery::form_field("other=1&token=+x+", "token") == " x ");
    assert(nxgallery::form_field("other=1", "token").empty());
    assert(nxgallery::form_field("token", "token").empty());
}

void release_version_contract() {
    assert(nxgallery::is_newer_release("0.1.0", "v0.1.1"));
    assert(nxgallery::is_newer_release("1.9.9", "2.0.0"));
    assert(!nxgallery::is_newer_release("0.1.0", "0.1.0"));
    assert(!nxgallery::is_newer_release("1.0.0", "0.99.99"));
    assert(!nxgallery::is_newer_release("1.0.0", "v1.1.0-beta.1"));
    assert(!nxgallery::is_newer_release("development", "1.0.0"));
}

}  // namespace

int main() {
    controller_flow();
    grid_multi_select_flow();
    telegram_batching_flow();
    telegram_batching_stops_on_failure();
    telegram_batching_uses_singleton_remainder();
    telegram_batching_splits_by_request_size();
    telegram_batching_stops_on_cancellation();
    grid_boundaries();
    config_contract();
    serialize_contract();
    shared_token_contract();
    http_request_contract();
    form_decode_contract();
    release_version_contract();
    album_scan_contract();
    album_limit_keeps_newest();
    return 0;
}
