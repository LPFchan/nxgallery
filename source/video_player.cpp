#include <nxgallery/horizon_album.hpp>
#include <nxgallery/video_player.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nxgallery {

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
        auto finish = [&] {
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
        decoded_frames = 0;
        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=decoder_ready width=%d height=%d nominal_fps_milli=%u pacing=timestamp\n",
                    codec->width, codec->height,
                    static_cast<unsigned int>(1000.0 / seconds_per_frame));

        while (!stop_requested && av_read_frame(format, packet) >= 0) {
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
                    frame_position_ms = std::max(last_position_ms, frame_position_ms);
                    if (playback_duration_ms > 0) {
                        frame_position_ms = std::min(frame_position_ms,
                                                     playback_duration_ms.load());
                    }
                    while (!stop_requested) {
                        std::unique_lock<std::mutex> lock(mutex);
                        if (is_paused) {
                            const auto pause_started = std::chrono::steady_clock::now();
                            wake.wait(lock, [this] { return stop_requested || !is_paused; });
                            if (!stop_requested) {
                                playback_origin += std::chrono::steady_clock::now() - pause_started;
                            }
                            continue;
                        }
                        const auto target = playback_origin +
                            std::chrono::milliseconds(frame_position_ms);
                        if (!wake.wait_until(lock, target, [this] {
                                return stop_requested || is_paused;
                            })) break;
                    }
                    if (stop_requested) break;
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
                        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=first_frame\n");
                    }
                }
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
    impl_->set_message(impl_->is_paused ? "Paused" : "Playing");
    std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=%s\n",
                impl_->is_paused ? "paused" : "playing");
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
