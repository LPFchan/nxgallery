#!/bin/sh
set -eu

repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
example="$repo_root/telegram-bot.conf.example"

[ -s "$repo_root/payload/switch/nxgallery/openssl/cert.pem" ]
[ -s "$repo_root/icon.jpg" ]
grep -Fq 'export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp' "$repo_root/Makefile"
grep -Fq '__nx_heap_size = 256ULL * 1024ULL * 1024ULL' "$repo_root/source/main.cpp"
grep -Fq 'options.AddDefaultSharedFont(PlSharedFontType_KO);' "$repo_root/source/main.cpp"
grep -Fq 'scan_horizon_album' "$repo_root/source/main.cpp"
grep -Fq 'NXGALLERY_PROBE_RESULT result=pass sd=pass playback=pass chats=pass photo=skip video=skip' \
    "$repo_root/scripts/run-hardware-probe.sh"
grep -Fq 'NXGALLERY_PROBE_SEND_MEDIA' "$repo_root/scripts/run-hardware-probe.sh"
grep -Fq 'NXGALLERY_RUN_MODE' "$repo_root/scripts/run-hardware-probe.sh"
grep -Fq 'artifacts/hardware-probe-last.log' "$repo_root/scripts/run-hardware-probe.sh"
grep -Fq 'sdmc:/switch/nxgallery/crash.log' "$repo_root/source/crash_diagnostics.cpp"
[ -x "$repo_root/scripts/run-hardware-probe.sh" ]
sh -n "$repo_root/scripts/run-hardware-probe.sh"
grep -q '^discover_chats=true$' "$example"
! grep -q '^chat=' "$example"
grep -Fq 'sdmc:/switch/nxgallery/telegram-state.json' "$repo_root/source/telegram_bot.cpp"
grep -Fq 'CURLOPT_XFERINFOFUNCTION' "$repo_root/source/telegram_bot.cpp"
grep -Fq 'start_chat_refresh();' "$repo_root/source/gallery_app.cpp"
grep -Fq 'kNxTorrentTelegramConfigPath' "$repo_root/source/main.cpp"
grep -Fq 'controller_.screen() == Screen::ChatPicker' "$repo_root/source/gallery_app.cpp"
grep -Fq 'open_token_setup(true);' "$repo_root/source/gallery_app.cpp"
grep -Fq 'start_album_scan();' "$repo_root/source/gallery_app.cpp"
grep -Fq 'kThumbnailFadeFrames' "$repo_root/source/gallery_app.cpp"
! grep -Fq 'kMaximumMultiSelect' "$repo_root/include/nxgallery/gallery_controller.hpp"
grep -Fq 'kMaximumTelegramBatchItems = 10' "$repo_root/include/nxgallery/telegram_batches.hpp"
grep -Fq 'send_telegram_batches(' "$repo_root/source/gallery_app.cpp"
grep -Fq 'sendMediaGroup' "$repo_root/source/telegram_bot.cpp"
telegram_source="$repo_root/source/telegram_bot.cpp"
send_media_source=$(sed -n '/BotResult TelegramBot::send_media(/,/^}/p' "$telegram_source")
send_group_source=$(sed -n '/BotResult TelegramBot::send_media_group(/,/^}/p' "$telegram_source")
printf '%s\n' "$send_media_source" | grep -Fq \
    'validated_video_thumbnail(media, thumbnail_path)'
printf '%s\n' "$send_media_source" | grep -Fq 'curl_mime_name(part, "thumbnail");'
printf '%s\n' "$send_media_source" | grep -Fq 'thumbnail_path.c_str()'
printf '%s\n' "$send_media_source" | grep -Fq 'curl_mime_type(part, "image/jpeg");'
printf '%s\n' "$send_group_source" | grep -Fq '"attach://thumbnail"'
printf '%s\n' "$send_group_source" | grep -Fq 'entry, "thumbnail",'
printf '%s\n' "$send_group_source" | grep -Fq '"thumbnail" + std::to_string(index)'
printf '%s\n' "$send_group_source" | grep -Fq 'thumbnail_paths[index].c_str()'
printf '%s\n' "$send_group_source" | grep -Fq 'curl_mime_type(part, "image/jpeg");'
grep -Fq 'materialize_thumbnail_path(media, path, error)' "$telegram_source"
grep -Fq 'kMaximumThumbnailBytes = 200U * 1024U' "$telegram_source"
! grep -Fq 'supports_streaming' "$telegram_source"
grep -Fq 'APP_VERSION ?= 0.1.5' "$repo_root/Makefile"
grep -Fq 'HidNpadButton_Minus' "$repo_root/source/gallery_app.cpp"
grep -Fq 'HidNpadButton_StickLLeft, HidNpadButton_StickLRight' "$repo_root/source/gallery_app.cpp"
grep -Fq 'viewer_browse_fire' "$repo_root/source/gallery_app.cpp"
grep -Fq 'void seek_relative(std::int64_t delta_ms);' "$repo_root/include/nxgallery/video_player.hpp"
grep -Fq 'av_seek_frame(' "$repo_root/source/video_player.cpp"
grep -Fq 'avcodec_flush_buffers(codec);' "$repo_root/source/video_player.cpp"
grep -Fq 'SDL_ClearQueuedAudio(device);' "$repo_root/source/video_player.cpp"
[ -x "$repo_root/scripts/build-switch-ffmpeg.sh" ]
sh -n "$repo_root/scripts/build-switch-ffmpeg.sh"
grep -Fq 'PLAYBACK_PREFIX must provide an FFmpeg libavcodec with the AAC decoder enabled' \
    "$repo_root/Makefile"
grep -Fq -- '--enable-decoder=aac' "$repo_root/scripts/build-switch-ffmpeg.sh"
grep -Fq 'ff_aac_decoder' "$repo_root/scripts/build-switch-ffmpeg.sh"
grep -Fq ' Update' "$repo_root/source/gallery_app.cpp"
grep -Fq 'https://api.github.com/repos/LPFchan/nxgallery/releases/latest' \
    "$repo_root/include/nxgallery/release_update.hpp"
grep -Fq 'constexpr char kReleaseAssetName[] = "nxgallery.nro"' \
    "$repo_root/include/nxgallery/release_update.hpp"
grep -Fq 'actual_digest != release.sha256' "$repo_root/source/release_update.cpp"
grep -Fq 'report_update_transfer' "$repo_root/source/release_update.cpp"
grep -Fq 'installed_nro_path(argc, argv)' "$repo_root/source/main.cpp"
! grep -Fq '"build " __DATE__' "$repo_root/source/gallery_app.cpp"
! grep -Fq 'No new chats; showing saved destinations' "$repo_root/source/telegram_bot.cpp"
! grep -Fq 'Configured chats loaded' "$repo_root/source/telegram_bot.cpp"
! grep -Fq 'exit_to_hbmenu' "$repo_root/source/gallery_app.cpp"
! grep -Fq '(void)save_state' "$repo_root/source/telegram_bot.cpp"
! grep -Eq '^bot_token=[0-9]{5,}:[A-Za-z0-9_-]{20,}$' "$example"
! rg -l -uu '^[[:space:]]*(api_hash|api_id)[[:space:]]*=' "$repo_root" \
    -g '!.secrets/**' -g '!records/**' >/dev/null
printf 'nxgallery release contract passed\n'
