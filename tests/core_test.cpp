#include <nxgallery/album_index.hpp>
#include <nxgallery/gallery_controller.hpp>
#include <nxgallery/telegram_config.hpp>

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
    assert(request->media.kind == nxgallery::MediaKind::Video);
    assert(request->chat.id == 1);
    assert(controller.screen() == nxgallery::Screen::Sending);
    controller.finish_share(true, "Sent");
    assert(controller.screen() == nxgallery::Screen::Result);
    assert(controller.share_succeeded());
    controller.handle(nxgallery::Action::Back);
    assert(controller.screen() == nxgallery::Screen::Viewer);
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

}  // namespace

int main() {
    controller_flow();
    grid_boundaries();
    config_contract();
    album_scan_contract();
    album_limit_keeps_newest();
    return 0;
}
