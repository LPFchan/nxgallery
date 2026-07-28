#pragma once

#include <nxgallery/model.hpp>
#include <nxgallery/telegram_batches.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nxgallery {

constexpr std::uint64_t kMaximumMergedVideoBytes =
    kMaximumTelegramRequestMediaBytes;

inline std::string video_merge_selection_error(
    const std::vector<MediaItem> &media) {
    if (media.size() < 2) return "Select at least two videos to merge";
    std::uint64_t total = 0;
    for (const MediaItem &item : media) {
        if (item.kind != MediaKind::Video) {
            return "Only videos can be merged";
        }
        if (item.size == 0 || item.size > kMaximumMergedVideoBytes - total) {
            return "Merged video would exceed the 48 MiB Telegram limit";
        }
        total += item.size;
    }
    return {};
}

using VideoMergeProgress =
    std::function<bool(std::uint64_t current, std::uint64_t total)>;

struct VideoMergeResult {
    bool success{};
    MediaItem media;
    std::string message;
};

VideoMergeResult merge_videos(const std::vector<MediaItem> &media,
                              const std::string &output_path,
                              VideoMergeProgress progress) noexcept;

}  // namespace nxgallery
