#include <nxgallery/horizon_album.hpp>
#include <nxgallery/video_player.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

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
            set_message("Could not open video capture");
            char ffmpeg_error[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(open_result < 0 ? open_result : info_result,
                        ffmpeg_error, sizeof(ffmpeg_error));
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
        AVRational frame_rate = av_guess_frame_rate(format, stream, nullptr);
        double seconds_per_frame = frame_rate.num > 0 && frame_rate.den > 0
            ? static_cast<double>(frame_rate.den) / frame_rate.num : 1.0 / 30.0;
        if (seconds_per_frame <= 0.0 || seconds_per_frame > 1.0) seconds_per_frame = 1.0 / 30.0;
        const auto frame_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(seconds_per_frame));
        auto next_frame = std::chrono::steady_clock::now();
        decoded_frames = 0;
        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=decoder_ready width=%d height=%d fps_milli=%u\n",
                    codec->width, codec->height,
                    static_cast<unsigned int>(1000.0 / seconds_per_frame));

        while (!stop_requested && av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == stream_index &&
                avcodec_send_packet(codec, packet) >= 0) {
                while (!stop_requested && avcodec_receive_frame(codec, frame) >= 0) {
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        const bool was_paused = is_paused;
                        const auto pause_started = std::chrono::steady_clock::now();
                        wake.wait(lock, [this] { return stop_requested || !is_paused; });
                        if (!stop_requested && was_paused) {
                            next_frame += std::chrono::steady_clock::now() - pause_started;
                        }
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
                    const std::uint64_t frame_number = ++decoded_frames;
                    if (frame_number == 1) {
                        std::printf("NXGALLERY_DIAGNOSTIC event=video_playback state=first_frame\n");
                    }
                    next_frame += frame_period;
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_until(lock, next_frame, [this] {
                        return stop_requested || is_paused;
                    });
                }
            }
            av_packet_unref(packet);
        }
        if (!stop_requested) {
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
    if (impl_->texture != nullptr) SDL_DestroyTexture(impl_->texture);
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

std::string VideoPlayer::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->message;
}

}  // namespace nxgallery
