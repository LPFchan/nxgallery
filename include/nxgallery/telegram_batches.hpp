#pragma once

#include <nxgallery/telegram_bot.hpp>

#include <cstddef>
#include <functional>
#include <vector>

namespace nxgallery {

constexpr std::size_t kMaximumTelegramBatchItems = 10;

using TelegramSingleSender = std::function<BotResult(
    const MediaItem &, TelegramBot::TransferProgress)>;
using TelegramGroupSender = std::function<BotResult(
    const std::vector<MediaItem> &, TelegramBot::TransferProgress)>;

BotResult send_telegram_batches(
    const std::vector<MediaItem> &media,
    TelegramBot::TransferProgress progress,
    const TelegramSingleSender &send_single,
    const TelegramGroupSender &send_group) noexcept;

}  // namespace nxgallery
