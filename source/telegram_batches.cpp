#include <nxgallery/telegram_batches.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace nxgallery {
namespace {

std::uint64_t media_bytes(const std::vector<MediaItem> &media,
                          std::size_t begin, std::size_t end) {
    std::uint64_t total = 0;
    for (std::size_t index = begin; index < end; ++index) {
        total += media[index].size;
    }
    return total;
}

BotResult send_telegram_batches_impl(
    const std::vector<MediaItem> &media,
    TelegramBot::TransferProgress progress,
    const TelegramSingleSender &send_single,
    const TelegramGroupSender &send_group) {
    if (media.empty()) return {false, "No captures selected"};
    if (!send_single || !send_group) return {false, "Telegram is unavailable"};

    const std::uint64_t full_total = media_bytes(media, 0, media.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t reported_bytes = 0;
    std::size_t completed_batches = 0;
    bool cancelled = false;

    const auto report = [&](std::uint64_t current) {
        reported_bytes = std::max(reported_bytes, std::min(current, full_total));
        if (progress && !progress(reported_bytes, full_total)) {
            cancelled = true;
            return false;
        }
        return true;
    };

    if (!report(0)) return {false, "Transfer cancelled"};

    for (std::size_t begin = 0; begin < media.size();
         begin += kMaximumTelegramBatchItems) {
        const std::size_t end = std::min(
            begin + kMaximumTelegramBatchItems, media.size());
        const std::uint64_t batch_bytes = media_bytes(media, begin, end);
        auto batch_progress = [&](std::uint64_t current, std::uint64_t total) {
            std::uint64_t scaled = 0;
            if (total > 0) {
                const long double fraction = std::min<long double>(
                    1.0L, static_cast<long double>(current) / total);
                scaled = static_cast<std::uint64_t>(batch_bytes * fraction);
            } else {
                scaled = std::min(current, batch_bytes);
            }
            return report(completed_bytes + scaled);
        };

        BotResult result;
        if (end - begin == 1) {
            result = send_single(media[begin], std::move(batch_progress));
        } else {
            const std::vector<MediaItem> batch(media.begin() + begin,
                                                media.begin() + end);
            result = send_group(batch, std::move(batch_progress));
        }
        if (!result.success) {
            if (completed_batches == 0) return result;
            return {false, cancelled
                ? "Some captures were sent before transfer was cancelled"
                : "Some captures were sent; remaining transfer failed"};
        }

        completed_bytes += batch_bytes;
        ++completed_batches;
        if (!report(completed_bytes)) {
            return {false, "Some captures were sent before transfer was cancelled"};
        }
    }

    return {true, media.size() == 1
        ? "Capture sent"
        : std::to_string(media.size()) + " captures sent"};
}

}  // namespace

BotResult send_telegram_batches(
    const std::vector<MediaItem> &media,
    TelegramBot::TransferProgress progress,
    const TelegramSingleSender &send_single,
    const TelegramGroupSender &send_group) noexcept {
    try {
        return send_telegram_batches_impl(media, std::move(progress), send_single,
                                          send_group);
    } catch (...) {
        return {false, "Telegram upload failed"};
    }
}

}  // namespace nxgallery
