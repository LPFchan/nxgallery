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
constexpr std::uint64_t kAudioQueueLowWatermarkMs = 200;
constexpr std::uint64_t kAudioQueueHighWatermarkMs = 400;
constexpr std::size_t kAudioPrimeMaximumScannedPackets = 2048;
constexpr std::size_t kMaximumDecodedVideoFrames = 3;
constexpr std::size_t kMaximumQueuedVideoPackets = 120;
constexpr std::size_t kMaximumQueuedVideoBytes = 4U * 1024U * 1024U;
constexpr std::size_t kVideoPacketTarget = 12;
constexpr std::size_t kMaximumQueuedAudioPackets = 128;
constexpr std::size_t kMaximumQueuedAudioBytes = 1U * 1024U * 1024U;

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
        std::deque<AVPacket *> video_packets;
        std::deque<AVPacket *> audio_packets;
        std::size_t queued_video_packet_bytes = 0;
        std::size_t queued_audio_packet_bytes = 0;
        auto clear_video_packets = [&] {
            while (!video_packets.empty()) {
                AVPacket *saved = video_packets.front();
                video_packets.pop_front();
                av_packet_free(&saved);
            }
            queued_video_packet_bytes = 0;
        };
        auto clear_audio_packets = [&] {
            while (!audio_packets.empty()) {
                AVPacket *saved = audio_packets.front();
                audio_packets.pop_front();
                av_packet_free(&saved);
            }
            queued_audio_packet_bytes = 0;
        };
        auto finish = [&] {
            clear_video_packets();
            clear_audio_packets();
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
        const std::uint64_t frame_interval_ms = std::max<std::uint64_t>(
            1U, static_cast<std::uint64_t>(seconds_per_frame * 1000.0));
        struct DecodedVideoFrame {
            std::vector<std::uint8_t> pixels;
            std::uint64_t pts_ms{};
            int width{};
            int height{};
        };
        std::deque<DecodedVideoFrame> video_frames;
        auto playback_origin = std::chrono::steady_clock::now();
        std::uint64_t discard_video_before_ms = 0;
        bool discarding_seek_preroll = false;
        bool audio_started = false;
        bool audio_queue_underrun_active = false;
        bool first_video_after_prime = false;
        bool first_frame_presented = false;
        bool refilling_audio = false;
        std::uint64_t total_audio_bytes_queued = 0;
        std::uint64_t dropped_video_frames = 0;
        std::uint64_t dropped_video_packets = 0;
        std::uint64_t presented_video_frames = 0;
        std::uint64_t fallback_video_pts_ms = 0;
        std::uint64_t next_playback_report_ms = 1000;
        const std::int64_t video_start_us = av_rescale_q(
            stream_start, stream->time_base, AV_TIME_BASE_Q);
        std::optional<std::int64_t> first_video_pts_ms;
        std::optional<std::int64_t> first_audio_pts_ms;
        std::int64_t fallback_audio_pts_ms = 0;
        const std::uint64_t audio_bytes_per_second =
            obtained_audio.freq > 0 && obtained_audio.channels > 0 ?
            static_cast<std::uint64_t>(obtained_audio.freq) *
                static_cast<std::uint64_t>(obtained_audio.channels) *
                static_cast<std::uint64_t>(SDL_AUDIO_BITSIZE(obtained_audio.format) / 8U) : 0U;

        auto queued_audio_bytes = [&]() -> std::uint64_t {
            const SDL_AudioDeviceID device = audio_device.load();
            return device != 0 ? SDL_GetQueuedAudioSize(device) : 0U;
        };
        auto audio_bytes_to_ms = [&](std::uint64_t bytes) -> std::uint64_t {
            return audio_bytes_per_second > 0 ? bytes * 1000U / audio_bytes_per_second : 0U;
        };
        auto queued_audio_ms = [&]() -> std::uint64_t {
            return audio_bytes_to_ms(queued_audio_bytes());
        };
        auto audio_clock_ms = [&]() -> std::uint64_t {
            if (!audio_started || !first_audio_pts_ms || audio_bytes_per_second == 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - playback_origin).count();
                return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
            }
            const std::uint64_t queued = queued_audio_bytes();
            const std::uint64_t played = total_audio_bytes_queued > queued ?
                total_audio_bytes_queued - queued : 0U;
            const std::int64_t clock = *first_audio_pts_ms +
                static_cast<std::int64_t>(audio_bytes_to_ms(played));
            return clock > 0 ? static_cast<std::uint64_t>(clock) : 0U;
        };

        auto packet_video_pts_ms = [&](const AVPacket *value)
                -> std::optional<std::int64_t> {
            const std::int64_t timestamp = value->pts != AV_NOPTS_VALUE ?
                value->pts : value->dts;
            if (timestamp == AV_NOPTS_VALUE) return std::nullopt;
            return av_rescale_q(timestamp - stream_start, stream->time_base,
                                AVRational{1, 1000});
        };

        auto enqueue_video_packet = [&](const AVPacket *value) {
            const std::size_t packet_bytes = value->size > 0 ?
                static_cast<std::size_t>(value->size) : 0U;
            if (video_packets.size() >= kMaximumQueuedVideoPackets ||
                packet_bytes > kMaximumQueuedVideoBytes -
                    std::min(queued_video_packet_bytes, kMaximumQueuedVideoBytes)) {
                ++dropped_video_packets;
                return;
            }
            AVPacket *saved = av_packet_clone(value);
            if (saved == nullptr) {
                ++dropped_video_packets;
                return;
            }
            video_packets.push_back(saved);
            queued_video_packet_bytes += packet_bytes;
        };

        auto enqueue_audio_packet = [&](const AVPacket *value) -> bool {
            const std::size_t packet_bytes = value->size > 0 ?
                static_cast<std::size_t>(value->size) : 0U;
            if (audio_packets.size() >= kMaximumQueuedAudioPackets ||
                packet_bytes > kMaximumQueuedAudioBytes -
                    std::min(queued_audio_packet_bytes, kMaximumQueuedAudioBytes)) {
                return false;
            }
            AVPacket *saved = av_packet_clone(value);
            if (saved == nullptr) return false;
            audio_packets.push_back(saved);
            queued_audio_packet_bytes += packet_bytes;
            return true;
        };

        auto decode_audio_packet = [&](AVPacket *value) -> std::uint64_t {
            if (audio_device.load() == 0 || audio_codec == nullptr ||
                avcodec_send_packet(audio_codec, value) < 0) {
                return 0U;
            }
            const std::uint64_t bytes = queue_audio(
                audio_codec, resampler, audio_frame, audio_stream,
                video_start_us, discard_video_before_ms,
                first_audio_pts_ms, fallback_audio_pts_ms);
            total_audio_bytes_queued += bytes;
            return bytes;
        };

        auto decode_video_packet = [&](AVPacket *value) {
            if (avcodec_send_packet(codec, value) < 0) return;
            while (!stop_requested && pending_seek_delta_ms.load() == 0 &&
                   avcodec_receive_frame(codec, frame) >= 0) {
                const std::uint64_t frame_number = decoded_frames.load() + 1U;
                std::uint64_t frame_position_ms = fallback_video_pts_ms;
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    const std::int64_t elapsed_ms = av_rescale_q(
                        std::max<std::int64_t>(0, frame->best_effort_timestamp - stream_start),
                        stream->time_base, AVRational{1, 1000});
                    frame_position_ms = static_cast<std::uint64_t>(elapsed_ms);
                }
                fallback_video_pts_ms = frame_position_ms + frame_interval_ms;
                decoded_frames = frame_number;
                if (discarding_seek_preroll && frame_position_ms < discard_video_before_ms) {
                    continue;
                }
                if (discarding_seek_preroll) discarding_seek_preroll = false;
                if (playback_duration_ms > 0) {
                    frame_position_ms = std::min(frame_position_ms,
                                                 playback_duration_ms.load());
                }
                if (video_frames.size() >= kMaximumDecodedVideoFrames) {
                    ++dropped_video_frames;
                    continue;
                }
                const std::size_t stride = static_cast<std::size_t>(codec->width) * 4U;
                DecodedVideoFrame decoded;
                decoded.pixels.resize(stride * static_cast<std::size_t>(codec->height));
                decoded.pts_ms = frame_position_ms;
                decoded.width = codec->width;
                decoded.height = codec->height;
                std::uint8_t *outputs[] = {decoded.pixels.data(), nullptr, nullptr, nullptr};
                int output_strides[] = {static_cast<int>(stride), 0, 0, 0};
                sws_scale(scaler, frame->data, frame->linesize, 0, codec->height,
                          outputs, output_strides);
                video_frames.push_back(std::move(decoded));
            }
        };

        auto present_due_video = [&](std::uint64_t wall_clock_ms) {
            std::optional<DecodedVideoFrame> selected;
            while (!video_frames.empty() &&
                   video_frames.front().pts_ms <= wall_clock_ms) {
                const std::uint64_t lateness_ms =
                    wall_clock_ms - video_frames.front().pts_ms;
                if (lateness_ms <= frame_interval_ms) {
                    selected = std::move(video_frames.front());
                    video_frames.pop_front();
                    break;
                }
                video_frames.pop_front();
                ++dropped_video_frames;
            }
            if (!selected) return;
            const std::uint64_t frame_pts_ms = selected->pts_ms;
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending_pixels = std::move(selected->pixels);
                pending_width = selected->width;
                pending_height = selected->height;
                pending_frame = true;
            }
            ++presented_video_frames;
            playback_position_ms = frame_pts_ms;
            if (first_video_after_prime) {
                first_video_after_prime = false;
                std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=video_sync_start video_pts_ms=%llu audio_pts_ms=%lld wall_clock_ms=%llu audio_clock_ms=%llu\n",
                            static_cast<unsigned long long>(frame_pts_ms),
                            static_cast<long long>(first_audio_pts_ms.value_or(-1)),
                            static_cast<unsigned long long>(wall_clock_ms),
                            static_cast<unsigned long long>(audio_clock_ms()));
            }
            if (!first_frame_presented) {
                first_frame_presented = true;
                std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=first_frame video_pts_ms=%llu wall_clock_ms=%llu audio_clock_ms=%llu queued_ms=%llu\n",
                            static_cast<unsigned long long>(frame_pts_ms),
                            static_cast<unsigned long long>(wall_clock_ms),
                            static_cast<unsigned long long>(audio_clock_ms()),
                            static_cast<unsigned long long>(queued_audio_ms()));
            }
        };

        auto prime_audio = [&](std::uint64_t target_ms) {
            clear_video_packets();
            clear_audio_packets();
            video_frames.clear();
            first_video_pts_ms.reset();
            first_audio_pts_ms.reset();
            fallback_audio_pts_ms = static_cast<std::int64_t>(target_ms);
            fallback_video_pts_ms = target_ms;
            total_audio_bytes_queued = 0;
            audio_queue_underrun_active = false;
            refilling_audio = false;
            next_playback_report_ms = target_ms + 1000U;
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
            std::size_t scanned_packets = 0;

            while (!stop_requested && pending_seek_delta_ms.load() == 0 &&
                   scanned_packets < kAudioPrimeMaximumScannedPackets) {
                if (queued_audio_ms() >= kAudioPrimeTargetMs) break;
                if (av_read_frame(format, packet) < 0) break;
                ++scanned_packets;
                if (packet->stream_index == stream_index) {
                    if (!first_video_pts_ms) first_video_pts_ms = packet_video_pts_ms(packet);
                    enqueue_video_packet(packet);
                } else if (packet->stream_index == audio_index) {
                    const std::uint64_t bytes = decode_audio_packet(packet);
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
                clear_video_packets();
                clear_audio_packets();
                video_frames.clear();
                return false;
            }

            const std::uint64_t queued_bytes = queued_audio_bytes();
            const std::uint64_t queued_ms = queued_audio_ms();
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
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_primed target_ms=%llu first_video_pts_ms=%lld first_audio_pts_ms=%lld queued_ms=%llu first_audio_delay_ms=%llu queued_video_packets=%zu queued_video_bytes=%zu alignment_ms=%lld\n",
                        static_cast<unsigned long long>(target_ms),
                        static_cast<long long>(first_video_pts_ms.value_or(-1)),
                        static_cast<long long>(*first_audio_pts_ms),
                        static_cast<unsigned long long>(queued_ms),
                        static_cast<unsigned long long>(first_audio_delay_ms.value_or(0)),
                        video_packets.size(),
                        queued_video_packet_bytes,
                        static_cast<long long>(
                            first_video_pts_ms.value_or(*first_audio_pts_ms) -
                            *first_audio_pts_ms));
            return true;
        };

        decoded_frames = 0;
        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=decoder_ready width=%d height=%d nominal_fps_milli=%u pacing=wall_clock audio_low_ms=%llu audio_high_ms=%llu video_frame_limit=%zu video_packet_target=%zu video_packet_limit=%zu video_packet_bytes_limit=%zu audio_packet_limit=%zu audio_packet_bytes_limit=%zu audio_bytes_per_second=%llu\n",
                    codec->width, codec->height,
                    static_cast<unsigned int>(1000.0 / seconds_per_frame),
                    static_cast<unsigned long long>(kAudioQueueLowWatermarkMs),
                    static_cast<unsigned long long>(kAudioQueueHighWatermarkMs),
                    kMaximumDecodedVideoFrames,
                    kVideoPacketTarget,
                    kMaximumQueuedVideoPackets,
                    kMaximumQueuedVideoBytes,
                    kMaximumQueuedAudioPackets,
                    kMaximumQueuedAudioBytes,
                    static_cast<unsigned long long>(audio_bytes_per_second));

        if (!prime_audio(0)) {
            finish();
            return;
        }

        bool reached_end = false;
        bool input_eof = false;
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
                    clear_video_packets();
                    clear_audio_packets();
                    video_frames.clear();
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        pending_pixels.clear();
                        pending_frame = false;
                    }
                    avcodec_flush_buffers(codec);
                    if (audio_codec != nullptr) avcodec_flush_buffers(audio_codec);
                    if (resampler != nullptr) {
                        swr_close(resampler);
                        swr_init(resampler);
                    }
                    av_packet_unref(packet);
                    playback_position_ms = target_ms;
                    discard_video_before_ms = target_ms;
                    discarding_seek_preroll = true;
                    input_eof = false;
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

            if (is_paused) {
                const auto pause_started = std::chrono::steady_clock::now();
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] {
                    return stop_requested || !is_paused ||
                        pending_seek_delta_ms.load() != 0;
                });
                if (!stop_requested) {
                    playback_origin += std::chrono::steady_clock::now() - pause_started;
                }
                continue;
            }

            const auto wall_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - playback_origin).count();
            const std::uint64_t wall_clock_ms = wall_elapsed > 0 ?
                static_cast<std::uint64_t>(wall_elapsed) : 0U;
            const std::uint64_t audio_position_ms = audio_clock_ms();
            present_due_video(wall_clock_ms);
            playback_position_ms = playback_duration_ms > 0 ?
                std::min(wall_clock_ms, playback_duration_ms.load()) : wall_clock_ms;
            const std::uint64_t current_queued_ms = queued_audio_ms();
            if (audio_started && current_queued_ms == 0 && !input_eof) {
                if (!audio_queue_underrun_active) {
                    audio_queue_underrun_active = true;
                    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_queue_underrun wall_clock_ms=%llu audio_clock_ms=%llu video_queue_frames=%zu video_queue_packets=%zu dropped_video_frames=%llu dropped_video_packets=%llu\n",
                                static_cast<unsigned long long>(wall_clock_ms),
                                static_cast<unsigned long long>(audio_position_ms),
                                video_frames.size(),
                                video_packets.size(),
                                static_cast<unsigned long long>(dropped_video_frames),
                                static_cast<unsigned long long>(dropped_video_packets));
                }
            } else if (audio_queue_underrun_active && current_queued_ms > 0) {
                audio_queue_underrun_active = false;
                std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=audio_queue_refilled wall_clock_ms=%llu audio_clock_ms=%llu queued_ms=%llu\n",
                            static_cast<unsigned long long>(wall_clock_ms),
                            static_cast<unsigned long long>(audio_position_ms),
                            static_cast<unsigned long long>(current_queued_ms));
            }
            if (wall_clock_ms >= next_playback_report_ms) {
                std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=playback_clock wall_clock_ms=%llu audio_clock_ms=%llu av_delta_ms=%lld queued_ms=%llu audio_queue_packets=%zu audio_queue_packet_bytes=%zu video_queue_frames=%zu video_queue_packets=%zu video_queue_bytes=%zu dropped_video_frames=%llu dropped_video_packets=%llu\n",
                            static_cast<unsigned long long>(wall_clock_ms),
                            static_cast<unsigned long long>(audio_position_ms),
                            static_cast<long long>(audio_position_ms) -
                                static_cast<long long>(wall_clock_ms),
                            static_cast<unsigned long long>(current_queued_ms),
                            audio_packets.size(),
                            queued_audio_packet_bytes,
                            video_frames.size(),
                            video_packets.size(),
                            queued_video_packet_bytes,
                            static_cast<unsigned long long>(dropped_video_frames),
                            static_cast<unsigned long long>(dropped_video_packets));
                next_playback_report_ms = wall_clock_ms + 1000U;
            }

            if (audio_started) {
                if (current_queued_ms < kAudioQueueLowWatermarkMs) refilling_audio = true;
                if (current_queued_ms >= kAudioQueueHighWatermarkMs) refilling_audio = false;
            }
            if (audio_started && refilling_audio && !audio_packets.empty()) {
                AVPacket *saved = audio_packets.front();
                audio_packets.pop_front();
                const std::size_t saved_bytes = saved->size > 0 ?
                    static_cast<std::size_t>(saved->size) : 0U;
                queued_audio_packet_bytes -=
                    std::min(queued_audio_packet_bytes, saved_bytes);
                decode_audio_packet(saved);
                av_packet_free(&saved);
                continue;
            }
            if (!video_packets.empty() &&
                video_frames.size() < kMaximumDecodedVideoFrames &&
                (!audio_started || !refilling_audio || input_eof)) {
                AVPacket *saved = video_packets.front();
                video_packets.pop_front();
                const std::size_t saved_bytes = saved->size > 0 ?
                    static_cast<std::size_t>(saved->size) : 0U;
                queued_video_packet_bytes -=
                    std::min(queued_video_packet_bytes, saved_bytes);
                decode_video_packet(saved);
                av_packet_free(&saved);
                continue;
            }
            const bool audio_packet_space =
                audio_packets.size() < kMaximumQueuedAudioPackets &&
                queued_audio_packet_bytes < kMaximumQueuedAudioBytes;
            const bool video_packet_space =
                video_packets.size() < kMaximumQueuedVideoPackets &&
                queued_video_packet_bytes < kMaximumQueuedVideoBytes;
            const bool needs_audio_packet = audio_started && refilling_audio &&
                audio_packets.empty();
            const bool needs_video_packet = video_packets.size() < kVideoPacketTarget;
            const bool should_demux = !input_eof &&
                (audio_packet_space || current_queued_ms < kAudioQueueHighWatermarkMs) &&
                (needs_audio_packet || (needs_video_packet && video_packet_space));
            if (!should_demux) {
                if (input_eof && audio_packets.empty() &&
                    (!audio_started || current_queued_ms == 0)) {
                    reached_end = video_packets.empty() && video_frames.empty();
                    if (reached_end) continue;
                }
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(2), [this] {
                    return stop_requested || is_paused ||
                        pending_seek_delta_ms.load() != 0;
                });
                continue;
            }

            const int read_result = av_read_frame(format, packet);
            if (read_result < 0) {
                input_eof = true;
                av_packet_unref(packet);
                continue;
            }
            if (packet->stream_index == stream_index) {
                if (!first_video_pts_ms) {
                    const auto packet_pts_ms = packet_video_pts_ms(packet);
                    if (packet_pts_ms) {
                        first_video_pts_ms = packet_pts_ms;
                        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=video_packet_start video_pts_ms=%lld wall_clock_ms=%llu\n",
                                    static_cast<long long>(*packet_pts_ms),
                                    static_cast<unsigned long long>(wall_clock_ms));
                    }
                }
                enqueue_video_packet(packet);
            } else if (audio_device.load() != 0 && packet->stream_index == audio_index) {
                if (audio_packets.empty() &&
                    (refilling_audio || current_queued_ms < kAudioQueueHighWatermarkMs)) {
                    decode_audio_packet(packet);
                } else if (!enqueue_audio_packet(packet)) {
                    decode_audio_packet(packet);
                }
            }
            av_packet_unref(packet);
        }
        if (!stop_requested) {
            if (playback_duration_ms > 0) playback_position_ms = playback_duration_ms.load();
            set_message("Playback finished");
            std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=finished frames=%llu presented_frames=%llu dropped_frames=%llu dropped_packets=%llu audio_clock_ms=%llu queued_ms=%llu\n",
                        static_cast<unsigned long long>(decoded_frames.load()),
                        static_cast<unsigned long long>(presented_video_frames),
                        static_cast<unsigned long long>(dropped_video_frames),
                        static_cast<unsigned long long>(dropped_video_packets),
                        static_cast<unsigned long long>(audio_clock_ms()),
                        static_cast<unsigned long long>(queued_audio_ms()));
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
