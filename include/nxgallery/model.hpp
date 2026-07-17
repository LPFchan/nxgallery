#pragma once

#include <cstdint>
#include <string>

namespace nxgallery {

enum class MediaKind { Photo, Video };

struct MediaItem {
    std::string path;
    std::string filename;
    MediaKind kind{MediaKind::Photo};
    std::int64_t modified_time{};
    std::uint64_t size{};
};

struct TelegramChat {
    std::int64_t id{};
    std::string title;
    std::string type;
};

inline bool operator==(const TelegramChat &left, const TelegramChat &right) {
    return left.id == right.id && left.title == right.title && left.type == right.type;
}

}  // namespace nxgallery
