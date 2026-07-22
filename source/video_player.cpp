#include <nxgallery/horizon_album.hpp>
#include <nxgallery/video_player.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nxgallery {

namespace {

constexpr std::uint64_t kAudioPrimeTargetMs = 160;
constexpr std::size_t kAudioPrimeMaximumVideoPackets = 240;
constexpr std::size_t kAudioPrimeMaximumVideoBytes = 8U * 1024U * 1024U;
constexpr std::size_t kAudioPrimeMaximumScannedPackets = 2048;

}  // namespace

struct VideoPlayer::Impl {
    explicit Impl(SDL_Renderer *value) : renderer(value) {}

    SDL_Renderer *renderer{};
    SDL_Texture *texture{};
    int texture_width{};
    int texture_height{};
    std::thread worker;
    std::atomic<bool> stop_requested{};
    std::atomic<bool> is_active{};
    std::atomic<bool> is_paused{};
    std::atomic<std::uint64_t> decoded_frames{};
    std::atomic<std::uint64_t> playback_position_ms{};
    std::atomic<std::uint64_t> playback_duration_ms{};
    std::atomic<std::int64_t> pending_seek_delta_ms{};
    std::atomic<SDL_AudioDeviceID> audio_device{0};
    std::atomic<bool> audio_primed{};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::uint8_t> pending_pixels;
    int pending_width{};
    int pending_height{};
    bool pending_frame{};
    std::string message;

    void set_message(std::string value) {
        std::lock_guard<std::mutex> lock(mutex);
        message = std::move(value);
    }

    // Drains every decoded audio frame in the codec into the SDL queue as
    // interleaved S16 stereo.
    std::uint64_t queue_audio(AVCodecContext *audio_codec, SwrContext *resampler,
                              AVFrame *audio_frame, AVStream *audio_stream,
                              std::int64_t video_start_us,
                              std::uint64_t discard_before_ms,
                              std::optional<std::int64_t> &first_audio_pts_ms,
                              std::int64_t &fallback_audio_pts_ms) {
        const SDL_AudioDeviceID device = audio_device.load();
        std::uint64_t queued_bytes = 0;
        while (!stop_requested && pending_seek_delta_ms.load() == 0 &&
               avcodec_receive_frame(audio_codec, audio_frame) >= 0) {
            const int capacity = swr_get_out_samples(resampler,
                                                     audio_frame->nb_samples);
            if (capacity <= 0) continue;
            std::vector<std::uint8_t> samples(
                static_cast<std::size_t>(capacity) * 4U);
            std::uint8_t *outputs[] = {samples.data()};
            const int converted = swr_convert(
                resampler, outputs, capacity,
                audio_frame->extended_data, audio_frame->nb_samples);
            if (converted > 0 && device != 0) {
                std::int64_t frame_pts_ms = fallback_audio_pts_ms;
                if (audio_frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    const std::int64_t frame_pts_us = av_rescale_q(
                        audio_frame->best_effort_timestamp,
                        audio_stream->time_base, AV_TIME_BASE_Q);
                    frame_pts_ms = (frame_pts_us - video_start_us) / 1000;
                }
                int skip_samples = 0;
                if (frame_pts_ms < static_cast<std::int64_t>(discard_before_ms)) {
                    const std::int64_t skip_ms =
                        static_cast<std::int64_t>(discard_before_ms) - frame_pts_ms;
                    skip_samples = static_cast<int>(std::min<std::int64_t>(
                        converted, (skip_ms * audio_codec->sample_rate + 999) / 1000));
                }
                const int playable_samples = converted - skip_samples;
                if (playable_samples <= 0) {
                    fallback_audio_pts_ms = frame_pts_ms +
                        static_cast<std::int64_t>(converted) * 1000 /
                            audio_codec->sample_rate;
                    continue;
                }
                const std::int64_t queued_pts_ms = frame_pts_ms +
                    static_cast<std::int64_t>(skip_samples) * 1000 /
                        audio_codec->sample_rate;
                const Uint32 bytes = static_cast<Uint32>(playable_samples) * 4U;
                if (SDL_QueueAudio(device, samples.data() + skip_samples * 4U,
                                   bytes) == 0) {
                    if (!first_audio_pts_ms) first_audio_pts_ms = queued_pts_ms;
                    queued_bytes += bytes;
                    fallback_audio_pts_ms = queued_pts_ms +
                        static_cast<std::int64_t>(playable_samples) * 1000 /
                            audio_codec->sample_rate;
                } else {
                    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_queue_failed reason=%s\n",
                                SDL_GetError());
                }
            }
        }
        return queued_bytes;
    }

    void decode(MediaItem media) {
        std::string path;
        std::string error;
        set_message("Loading video...");
        if (!materialize_media_path(media, path, error)) {
            set_message(std::move(error));
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=materialize_failed\n");
            is_active = false;
            return;
        }

        AVFormatContext *format = nullptr;
        AVCodecContext *codec = nullptr;
        SwsContext *scaler = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *frame = nullptr;
        AVCodecContext *audio_codec = nullptr;
        SwrContext *resampler = nullptr;
        AVFrame *audio_frame = nullptr;
        AVStream *audio_stream = nullptr;
        SDL_AudioSpec obtained_audio{};
        std::deque<AVPacket *> primed_video_packets;
        auto clear_primed_video_packets = [&] {
            while (!primed_video_packets.empty()) {
                AVPacket *saved = primed_video_packets.front();
                primed_video_packets.pop_front();
                av_packet_free(&saved);
            }
        };
        auto finish = [&] {
            clear_primed_video_packets();
            audio_primed = false;
            const SDL_AudioDeviceID device = audio_device.exchange(0);
            if (device != 0) {
                SDL_ClearQueuedAudio(device);
                SDL_CloseAudioDevice(device);
            }
            if (audio_frame != nullptr) av_frame_free(&audio_frame);
            if (resampler != nullptr) swr_free(&resampler);
            if (audio_codec != nullptr) avcodec_free_context(&audio_codec);
            if (frame != nullptr) av_frame_free(&frame);
            if (packet != nullptr) av_packet_free(&packet);
            if (scaler != nullptr) sws_freeContext(scaler);
            if (codec != nullptr) avcodec_free_context(&codec);
            if (format != nullptr) avformat_close_input(&format);
            is_active = false;
        };

        const std::string input_url = path.rfind("sdmc:/", 0) == 0
            ? "file:" + path : path;
        const int open_result = avformat_open_input(
            &format, input_url.c_str(), nullptr, nullptr);
        const int info_result = open_result < 0
            ? open_result : avformat_find_stream_info(format, nullptr);
        if (open_result < 0 || info_result < 0) {
            char ffmpeg_error[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(open_result < 0 ? open_result : info_result,
                        ffmpeg_error, sizeof(ffmpeg_error));
            set_message(std::string("Could not open video: ") + ffmpeg_error);
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=open_failed code=%d reason=%s\n",
                        open_result < 0 ? open_result : info_result, ffmpeg_error);
            finish();
            return;
        }
        const int stream_index = av_find_best_stream(
            format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index < 0) {
            set_message("Video capture has no decodable stream");
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=no_video_stream\n");
            finish();
            return;
        }
        AVStream *stream = format->streams[stream_index];
        std::int64_t duration_ms = 0;
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            duration_ms = av_rescale_q(stream->duration, stream->time_base,
                                       AVRational{1, 1000});
        } else if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
            duration_ms = av_rescale_q(format->duration, AV_TIME_BASE_Q,
                                       AVRational{1, 1000});
        }
        playback_duration_ms = duration_ms > 0
            ? static_cast<std::uint64_t>(duration_ms) : 0U;
        const std::int64_t stream_start = stream->start_time != AV_NOPTS_VALUE
            ? stream->start_time : 0;
        const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        codec = decoder == nullptr ? nullptr : avcodec_alloc_context3(decoder);
        if (codec == nullptr ||
            avcodec_parameters_to_context(codec, stream->codecpar) < 0 ||
            avcodec_open2(codec, decoder, nullptr) < 0) {
            set_message("Could not initialize video decoder");
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=decoder_failed codec=%d\n",
                        static_cast<int>(stream->codecpar->codec_id));
            finish();
            return;
        }
        scaler = sws_getContext(codec->width, codec->height, codec->pix_fmt,
                                codec->width, codec->height, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (scaler == nullptr || packet == nullptr || frame == nullptr) {
            set_message("Could not allocate video decoder buffers");
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=buffer_failed\n");
            finish();
            return;
        }

        // Audio is best-effort: playback stays video-only when any part of
        // the audio pipeline cannot start.
        const int audio_index = av_find_best_stream(
            format, AVMEDIA_TYPE_AUDIO, -1, stream_index, nullptr, 0);
        if (audio_index >= 0) {
            audio_stream = format->streams[audio_index];
            const AVCodec *audio_decoder =
                avcodec_find_decoder(audio_stream->codecpar->codec_id);
            audio_codec = audio_decoder == nullptr
                ? nullptr : avcodec_alloc_context3(audio_decoder);
            bool audio_ready = audio_codec != nullptr &&
                avcodec_parameters_to_context(audio_codec, audio_stream->codecpar) >= 0 &&
                avcodec_open2(audio_codec, audio_decoder, nullptr) >= 0 &&
                audio_codec->sample_rate > 0;
            if (audio_ready) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                audio_ready = swr_alloc_set_opts2(
                    &resampler, &stereo, AV_SAMPLE_FMT_S16,
                    audio_codec->sample_rate, &audio_codec->ch_layout,
                    audio_codec->sample_fmt, audio_codec->sample_rate,
                    0, nullptr) >= 0 && swr_init(resampler) >= 0;
            }
            if (audio_ready && SDL_WasInit(SDL_INIT_AUDIO) == 0) {
                audio_ready = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
            }
            if (audio_ready) {
                audio_frame = av_frame_alloc();
                audio_ready = audio_frame != nullptr;
            }
            if (audio_ready) {
                SDL_AudioSpec desired{};
                desired.freq = audio_codec->sample_rate;
                desired.format = AUDIO_S16SYS;
                desired.channels = 2;
                desired.samples = 2048;
                const SDL_AudioDeviceID device =
                    SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained_audio, 0);
                audio_ready = device != 0;
                if (audio_ready) {
                    audio_device = device;
                    audio_primed = false;
                    SDL_PauseAudioDevice(device, 1);
                    const std::uint64_t buffer_ms = obtained_audio.freq > 0 ?
                        static_cast<std::uint64_t>(obtained_audio.samples) * 1000U /
                            static_cast<std::uint64_t>(obtained_audio.freq) : 0U;
                    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_device nominal_rate=%d nominal_channels=%u nominal_format=%u nominal_samples=%u obtained_rate=%d obtained_channels=%u obtained_format=%u obtained_samples=%u obtained_buffer_ms=%llu\n",
                                desired.freq, static_cast<unsigned int>(desired.channels),
                                static_cast<unsigned int>(desired.format),
                                static_cast<unsigned int>(desired.samples),
                                obtained_audio.freq,
                                static_cast<unsigned int>(obtained_audio.channels),
                                static_cast<unsigned int>(obtained_audio.format),
                                static_cast<unsigned int>(obtained_audio.samples),
                                static_cast<unsigned long long>(buffer_ms));
                }
            }
            if (audio_ready) {
                std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_ready rate=%d paused_for_prime=true\n",
                            audio_codec->sample_rate);
            } else {
                std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_unavailable codec=%d reason=%s\n",
                            static_cast<int>(audio_stream->codecpar->codec_id),
                            SDL_GetError());
                if (audio_frame != nullptr) av_frame_free(&audio_frame);
                if (resampler != nullptr) swr_free(&resampler);
                if (audio_codec != nullptr) avcodec_free_context(&audio_codec);
            }
        }

        set_message(is_paused ? "Paused" : "Playing");
        AVRational frame_rate = stream->r_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0) {
            frame_rate = stream->avg_frame_rate;
        }
        if (frame_rate.num <= 0 || frame_rate.den <= 0) {
            frame_rate = av_guess_frame_rate(format, stream, nullptr);
        }
        double seconds_per_frame = frame_rate.num > 0 && frame_rate.den > 0
            ? static_cast<double>(frame_rate.den) / frame_rate.num : 1.0 / 30.0;
        if (seconds_per_frame <= 0.0 || seconds_per_frame > 1.0) seconds_per_frame = 1.0 / 30.0;
        auto playback_origin = std::chrono::steady_clock::now();
        std::uint64_t last_position_ms = 0;
        std::uint64_t discard_video_before_ms = 0;
        bool discarding_seek_preroll = false;
        bool audio_started = false;
        bool audio_queue_underrun_reported = false;
        bool first_video_after_prime = false;
        const std::int64_t video_start_us = av_rescale_q(
            stream_start, stream->time_base, AV_TIME_BASE_Q);
        std::optional<std::int64_t> first_video_pts_ms;
        std::optional<std::int64_t> first_audio_pts_ms;
        std::int64_t fallback_audio_pts_ms = 0;

        auto packet_video_pts_ms = [&](const AVPacket *value)
                -> std::optional<std::int64_t> {
            const std::int64_t timestamp = value->pts != AV_NOPTS_VALUE ?
                value->pts : value->dts;
            if (timestamp == AV_NOPTS_VALUE) return std::nullopt;
            return av_rescale_q(timestamp - stream_start, stream->time_base,
                                AVRational{1, 1000});
        };

        auto prime_audio = [&](std::uint64_t target_ms) {
            clear_primed_video_packets();
            first_video_pts_ms.reset();
            first_audio_pts_ms.reset();
            fallback_audio_pts_ms = static_cast<std::int64_t>(target_ms);
            audio_queue_underrun_reported = false;
            first_video_after_prime = true;
            const SDL_AudioDeviceID device = audio_device.load();
            if (device == 0 || audio_codec == nullptr || audio_stream == nullptr) {
                playback_origin = std::chrono::steady_clock::now() -
                    std::chrono::milliseconds(target_ms);
                audio_started = false;
                return true;
            }

            SDL_PauseAudioDevice(device, 1);
            SDL_ClearQueuedAudio(device);
            audio_started = false;
            audio_primed = false;
            const auto prime_started = std::chrono::steady_clock::now();
            std::optional<std::uint64_t> first_audio_delay_ms;
            std::size_t retained_video_bytes = 0;
            std::size_t scanned_packets = 0;
            bool retention_limit_reported = false;

            while (!stop_requested && pending_seek_delta_ms.load() == 0 &&
                   scanned_packets < kAudioPrimeMaximumScannedPackets) {
                const std::uint64_t queued_ms =
                    static_cast<std::uint64_t>(SDL_GetQueuedAudioSize(device)) * 1000U /
                    (static_cast<std::uint64_t>(audio_codec->sample_rate) * 4U);
                if (queued_ms >= kAudioPrimeTargetMs) break;
                if (av_read_frame(format, packet) < 0) break;
                ++scanned_packets;
                if (packet->stream_index == stream_index) {
                    if (!first_video_pts_ms) first_video_pts_ms = packet_video_pts_ms(packet);
                    const std::size_t packet_bytes = packet->size > 0 ?
                        static_cast<std::size_t>(packet->size) : 0U;
                    if (primed_video_packets.size() < kAudioPrimeMaximumVideoPackets &&
                        packet_bytes <= kAudioPrimeMaximumVideoBytes -
                            std::min(retained_video_bytes, kAudioPrimeMaximumVideoBytes)) {
                        AVPacket *saved = av_packet_clone(packet);
                        if (saved != nullptr) {
                            primed_video_packets.push_back(saved);
                            retained_video_bytes += packet_bytes;
                        }
                    } else if (!retention_limit_reported) {
                        retention_limit_reported = true;
                        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_prime_video_limit packets=%zu bytes=%zu\n",
                                    primed_video_packets.size(), retained_video_bytes);
                    }
                } else if (packet->stream_index == audio_index &&
                           avcodec_send_packet(audio_codec, packet) >= 0) {
                    const std::uint64_t bytes = queue_audio(
                        audio_codec, resampler, audio_frame, audio_stream,
                        video_start_us, target_ms, first_audio_pts_ms,
                        fallback_audio_pts_ms);
                    if (bytes > 0 && !first_audio_delay_ms) {
                        first_audio_delay_ms = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - prime_started).count());
                    }
                }
                av_packet_unref(packet);
            }
            av_packet_unref(packet);
            if (stop_requested || pending_seek_delta_ms.load() != 0) {
                clear_primed_video_packets();
                return false;
            }

            const std::uint64_t queued_bytes = SDL_GetQueuedAudioSize(device);
            const std::uint64_t queued_ms = queued_bytes * 1000U /
                (static_cast<std::uint64_t>(audio_codec->sample_rate) * 4U);
            if (queued_bytes == 0 || !first_audio_pts_ms) {
                playback_origin = std::chrono::steady_clock::now() -
                    std::chrono::milliseconds(target_ms);
                audio_started = false;
                audio_primed = false;
                std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_prime_unavailable target_ms=%llu scanned_packets=%zu\n",
                            static_cast<unsigned long long>(target_ms), scanned_packets);
                return true;
            }

            const auto synchronized_start = std::chrono::steady_clock::now();
            playback_origin = synchronized_start -
                std::chrono::milliseconds(*first_audio_pts_ms);
            audio_started = true;
            audio_primed = true;
            SDL_PauseAudioDevice(device, is_paused ? 1 : 0);
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_primed target_ms=%llu first_video_pts_ms=%lld first_audio_pts_ms=%lld queued_ms=%llu first_audio_delay_ms=%llu retained_video_packets=%zu alignment_ms=%lld\n",
                        static_cast<unsigned long long>(target_ms),
                        static_cast<long long>(first_video_pts_ms.value_or(-1)),
                        static_cast<long long>(*first_audio_pts_ms),
                        static_cast<unsigned long long>(queued_ms),
                        static_cast<unsigned long long>(first_audio_delay_ms.value_or(0)),
                        primed_video_packets.size(),
                        static_cast<long long>(
                            first_video_pts_ms.value_or(*first_audio_pts_ms) -
                            *first_audio_pts_ms));
            return true;
        };

        decoded_frames = 0;
        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=decoder_ready width=%d height=%d nominal_fps_milli=%u pacing=timestamp\n",
                    codec->width, codec->height,
                    static_cast<unsigned int>(1000.0 / seconds_per_frame));

        if (!prime_audio(0)) {
            finish();
            return;
        }

        bool reached_end = false;
        while (!stop_requested && !reached_end) {
            const std::int64_t seek_delta_ms = pending_seek_delta_ms.exchange(0);
            if (seek_delta_ms != 0) {
                const std::uint64_t current_ms = playback_position_ms.load();
                const std::uint64_t duration = playback_duration_ms.load();
                const std::uint64_t maximum_ms = duration > 0
                    ? std::min(duration, static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()))
                    : static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max());
                std::uint64_t target_ms = current_ms;
                if (seek_delta_ms < 0) {
                    const std::uint64_t magnitude = static_cast<std::uint64_t>(
                        -(seek_delta_ms + 1)) + 1U;
                    target_ms = magnitude >= current_ms ? 0U : current_ms - magnitude;
                } else {
                    const std::uint64_t magnitude =
                        static_cast<std::uint64_t>(seek_delta_ms);
                    target_ms = magnitude > maximum_ms - std::min(current_ms, maximum_ms)
                        ? maximum_ms : current_ms + magnitude;
                    target_ms = std::min(target_ms, maximum_ms);
                }
                const std::int64_t target_timestamp = stream_start + av_rescale_q(
                    static_cast<std::int64_t>(target_ms),
                    AVRational{1, 1000}, stream->time_base);
                const int seek_result = av_seek_frame(
                    format, stream_index, target_timestamp, AVSEEK_FLAG_BACKWARD);
                if (seek_result >= 0) {
                    const SDL_AudioDeviceID device = audio_device.load();
                    if (device != 0) {
                        audio_primed = false;
                        SDL_PauseAudioDevice(device, 1);
                        SDL_ClearQueuedAudio(device);
                    }
                    clear_primed_video_packets();
                    avcodec_flush_buffers(codec);
                    if (audio_codec != nullptr) avcodec_flush_buffers(audio_codec);
                    if (resampler != nullptr) {
                        swr_close(resampler);
                        swr_init(resampler);
                    }
                    av_packet_unref(packet);
                    playback_position_ms = target_ms;
                    last_position_ms = target_ms;
                    discard_video_before_ms = target_ms;
                    discarding_seek_preroll = true;
                    if (!prime_audio(target_ms)) continue;
                    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=seeked from_ms=%llu target_ms=%llu delta_ms=%lld\n",
                                static_cast<unsigned long long>(current_ms),
                                static_cast<unsigned long long>(target_ms),
                                static_cast<long long>(seek_delta_ms));
                } else {
                    char ffmpeg_error[AV_ERROR_MAX_STRING_SIZE]{};
                    av_strerror(seek_result, ffmpeg_error, sizeof(ffmpeg_error));
                    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=seek_failed target_ms=%llu code=%d reason=%s\n",
                                static_cast<unsigned long long>(target_ms),
                                seek_result, ffmpeg_error);
                }
            }

            int read_result = 0;
            if (!primed_video_packets.empty()) {
                AVPacket *saved = primed_video_packets.front();
                primed_video_packets.pop_front();
                av_packet_move_ref(packet, saved);
                av_packet_free(&saved);
            } else {
                read_result = av_read_frame(format, packet);
            }
            if (read_result < 0) {
                if (pending_seek_delta_ms.load() != 0) continue;
                while (!stop_requested && !is_paused &&
                       pending_seek_delta_ms.load() == 0 &&
                       audio_device.load() != 0 &&
                       SDL_GetQueuedAudioSize(audio_device.load()) > 0) {
                    SDL_Delay(30);
                }
                reached_end = pending_seek_delta_ms.load() == 0;
                continue;
            }
            if (packet->stream_index == stream_index &&
                avcodec_send_packet(codec, packet) >= 0) {
                while (!stop_requested && avcodec_receive_frame(codec, frame) >= 0) {
                    const std::uint64_t frame_number = decoded_frames.load() + 1U;
                    std::uint64_t frame_position_ms = static_cast<std::uint64_t>(
                        (frame_number - 1U) * seconds_per_frame * 1000.0);
                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        const std::int64_t elapsed_ms = av_rescale_q(
                            std::max<std::int64_t>(0, frame->best_effort_timestamp - stream_start),
                            stream->time_base, AVRational{1, 1000});
                        frame_position_ms = static_cast<std::uint64_t>(elapsed_ms);
                    }
                    if (discarding_seek_preroll &&
                        frame_position_ms < discard_video_before_ms) {
                        continue;
                    }
                    if (discarding_seek_preroll) {
                        discarding_seek_preroll = false;
                    }
                    frame_position_ms = std::max(last_position_ms, frame_position_ms);
                    if (playback_duration_ms > 0) {
                        frame_position_ms = std::min(frame_position_ms,
                                                     playback_duration_ms.load());
                    }
                    if (first_video_after_prime) {
                        first_video_after_prime = false;
                        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=video_sync_start video_pts_ms=%llu audio_pts_ms=%lld\n",
                                    static_cast<unsigned long long>(frame_position_ms),
                                    static_cast<long long>(first_audio_pts_ms.value_or(-1)));
                    }
                    const SDL_AudioDeviceID pacing_device = audio_device.load();
                    if (audio_started && pacing_device != 0 && !is_paused &&
                        SDL_GetQueuedAudioSize(pacing_device) == 0 &&
                        !audio_queue_underrun_reported) {
                        audio_queue_underrun_reported = true;
                        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_queue_underrun video_pts_ms=%llu\n",
                                    static_cast<unsigned long long>(frame_position_ms));
                    }
                    while (!stop_requested) {
                        std::unique_lock<std::mutex> lock(mutex);
                        if (is_paused) {
                            const auto pause_started = std::chrono::steady_clock::now();
                            wake.wait(lock, [this] {
                                return stop_requested || !is_paused ||
                                    pending_seek_delta_ms.load() != 0;
                            });
                            if (!stop_requested) {
                                playback_origin += std::chrono::steady_clock::now() - pause_started;
                            }
                            if (pending_seek_delta_ms.load() != 0) break;
                            continue;
                        }
                        const auto target = playback_origin +
                            std::chrono::milliseconds(frame_position_ms);
                        if (!wake.wait_until(lock, target, [this] {
                                return stop_requested || is_paused ||
                                    pending_seek_delta_ms.load() != 0;
                            })) break;
                        if (pending_seek_delta_ms.load() != 0) break;
                    }
                    if (stop_requested || pending_seek_delta_ms.load() != 0) break;
                    const std::size_t stride = static_cast<std::size_t>(codec->width) * 4U;
                    std::vector<std::uint8_t> pixels(
                        stride * static_cast<std::size_t>(codec->height));
                    std::uint8_t *outputs[] = {pixels.data(), nullptr, nullptr, nullptr};
                    int output_strides[] = {static_cast<int>(stride), 0, 0, 0};
                    sws_scale(scaler, frame->data, frame->linesize, 0, codec->height,
                              outputs, output_strides);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        pending_pixels = std::move(pixels);
                        pending_width = codec->width;
                        pending_height = codec->height;
                        pending_frame = true;
                    }
                    decoded_frames = frame_number;
                    playback_position_ms = frame_position_ms;
                    last_position_ms = frame_position_ms;
                    if (frame_number == 1) {
                        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=first_frame video_pts_ms=%llu\n",
                                    static_cast<unsigned long long>(frame_position_ms));
                    }
                }
            } else if (audio_device.load() != 0 &&
                       packet->stream_index == audio_index &&
                       avcodec_send_packet(audio_codec, packet) >= 0) {
                queue_audio(
                    audio_codec, resampler, audio_frame, audio_stream,
                    video_start_us, discard_video_before_ms,
                    first_audio_pts_ms, fallback_audio_pts_ms);
            }
            av_packet_unref(packet);
        }
        if (!stop_requested) {
            if (playback_duration_ms > 0) playback_position_ms = playback_duration_ms.load();
            set_message("Playback finished");
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=finished frames=%llu\n",
                        static_cast<unsigned long long>(decoded_frames.load()));
        }
        finish();
    }
};

VideoPlayer::VideoPlayer(SDL_Renderer *renderer) : impl_(std::make_unique<Impl>(renderer)) {
    av_log_set_level(AV_LOG_ERROR);
}

VideoPlayer::~VideoPlayer() {
    stop();
}

void VideoPlayer::play(const MediaItem &media) {
    stop();
    impl_->stop_requested = false;
    impl_->is_paused = false;
    impl_->is_active = true;
    impl_->set_message("Loading video...");
    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=loading\n");
    impl_->worker = std::thread([this, media] { impl_->decode(media); });
}

void VideoPlayer::toggle_pause() {
    if (!impl_->is_active) return;
    impl_->is_paused = !impl_->is_paused.load();
    const SDL_AudioDeviceID device = impl_->audio_device.load();
    if (device != 0 && impl_->audio_primed) {
        SDL_PauseAudioDevice(device, impl_->is_paused ? 1 : 0);
    }
    impl_->set_message(impl_->is_paused ? "Paused" : "Playing");
    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=%s\n",
                impl_->is_paused ? "paused" : "playing");
    impl_->wake.notify_all();
}

void VideoPlayer::seek_relative(std::int64_t delta_ms) {
    if (delta_ms == 0 || !impl_->is_active) return;
    std::int64_t pending = impl_->pending_seek_delta_ms.load();
    while (true) {
        std::int64_t combined = 0;
        if (delta_ms > 0 && pending >
            std::numeric_limits<std::int64_t>::max() - delta_ms) {
            combined = std::numeric_limits<std::int64_t>::max();
        } else if (delta_ms < 0 && pending <
                   std::numeric_limits<std::int64_t>::min() - delta_ms) {
            combined = std::numeric_limits<std::int64_t>::min();
        }
        else combined = pending + delta_ms;
        if (impl_->pending_seek_delta_ms.compare_exchange_weak(pending, combined)) break;
    }
    impl_->wake.notify_all();
}

void VideoPlayer::stop() {
    impl_->stop_requested = true;
    impl_->is_paused = false;
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->is_active = false;
    impl_->stop_requested = false;
    impl_->decoded_frames = 0;
    impl_->playback_position_ms = 0;
    impl_->playback_duration_ms = 0;
    impl_->pending_seek_delta_ms = 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pending_pixels.clear();
    impl_->pending_width = 0;
    impl_->pending_height = 0;
    impl_->pending_frame = false;
    if (impl_->texture != nullptr) SDL_DestroyTexture(impl_->texture);
    impl_->texture = nullptr;
    impl_->texture_width = 0;
    impl_->texture_height = 0;
}

void VideoPlayer::update_texture() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->pending_frame || impl_->renderer == nullptr) return;
    if (impl_->texture == nullptr || impl_->texture_width != impl_->pending_width ||
        impl_->texture_height != impl_->pending_height) {
        if (impl_->texture != nullptr) SDL_DestroyTexture(impl_->texture);
        impl_->texture = SDL_CreateTexture(
            impl_->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
            impl_->pending_width, impl_->pending_height);
        impl_->texture_width = impl_->pending_width;
        impl_->texture_height = impl_->pending_height;
    }
    if (impl_->texture != nullptr) {
        SDL_UpdateTexture(impl_->texture, nullptr, impl_->pending_pixels.data(),
                          impl_->pending_width * 4);
    }
    impl_->pending_frame = false;
}

SDL_Texture *VideoPlayer::texture() const noexcept { return impl_->texture; }
bool VideoPlayer::active() const noexcept { return impl_->is_active; }
bool VideoPlayer::paused() const noexcept { return impl_->is_paused; }
std::uint64_t VideoPlayer::frames_decoded() const noexcept {
    return impl_->decoded_frames.load();
}
std::uint64_t VideoPlayer::position_ms() const noexcept {
    return impl_->playback_position_ms.load();
}
std::uint64_t VideoPlayer::duration_ms() const noexcept {
    return impl_->playback_duration_ms.load();
}

std::string VideoPlayer::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->message;
}

}  // namespace nxgallery
