#include <nxgallery/horizon_album.hpp>
#include <nxgallery/video_merge.hpp>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nxgallery {
namespace {

std::string ffmpeg_error(const char *prefix, int code) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, detail, sizeof(detail));
    return std::string(prefix) + ": " + detail;
}

bool compatible_stream(const AVCodecParameters *left,
                       const AVCodecParameters *right) {
    if (left == nullptr || right == nullptr ||
        left->codec_type != right->codec_type ||
        left->codec_id != right->codec_id ||
        left->format != right->format ||
        left->profile != right->profile || left->level != right->level ||
        left->extradata_size != right->extradata_size) {
        return false;
    }
    if (left->extradata_size > 0) {
        if (left->extradata == nullptr || right->extradata == nullptr ||
            std::memcmp(left->extradata, right->extradata,
                        static_cast<std::size_t>(left->extradata_size)) != 0) {
            return false;
        }
    }
    if (left->codec_type == AVMEDIA_TYPE_VIDEO) {
        return left->width == right->width && left->height == right->height;
    }
    if (left->codec_type == AVMEDIA_TYPE_AUDIO) {
        return left->sample_rate == right->sample_rate &&
               left->ch_layout.nb_channels == right->ch_layout.nb_channels;
    }
    return false;
}

struct InputOwner {
    AVFormatContext *context{};
    ~InputOwner() {
        if (context != nullptr) avformat_close_input(&context);
    }
};

struct OutputOwner {
    AVFormatContext *context{};
    bool file_open{};
    ~OutputOwner() {
        if (context == nullptr) return;
        if (file_open) avio_closep(&context->pb);
        avformat_free_context(context);
    }
};

struct PacketOwner {
    AVPacket *packet{av_packet_alloc()};
    ~PacketOwner() { av_packet_free(&packet); }
};

VideoMergeResult failure(std::string message) {
    return {false, {}, std::move(message)};
}

}  // namespace

VideoMergeResult merge_videos(const std::vector<MediaItem> &media,
                              const std::string &output_path,
                              VideoMergeProgress progress) noexcept {
    try {
        const std::string selection_error = video_merge_selection_error(media);
        if (!selection_error.empty()) return failure(selection_error);
        if (output_path.empty()) return failure("Merged video path is unavailable");
        (void)std::remove(output_path.c_str());

        std::vector<std::string> paths;
        paths.reserve(media.size());
        std::uint64_t total_bytes = 0;
        for (const MediaItem &item : media) {
            std::string path;
            std::string error;
            if (!materialize_media_path(item, path, error)) {
                return failure(error.empty() ? "Could not read a selected video" :
                                               std::move(error));
            }
            struct stat status {};
            if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
                status.st_size <= 0) {
                return failure("A selected video is unavailable");
            }
            const std::uint64_t size =
                static_cast<std::uint64_t>(status.st_size);
            if (size > kMaximumMergedVideoBytes - total_bytes) {
                return failure(
                    "Merged video would exceed the 48 MiB Telegram limit");
            }
            total_bytes += size;
            paths.push_back(std::move(path));
        }
        if (progress && !progress(0, total_bytes)) {
            return failure("Transfer cancelled");
        }

        OutputOwner output;
        PacketOwner packet;
        if (packet.packet == nullptr) return failure("Could not prepare video merge");

        std::uint64_t processed_bytes = 0;
        std::int64_t output_offset_us = 0;
        bool header_written = false;
        for (std::size_t file_index = 0; file_index < paths.size(); ++file_index) {
            InputOwner input;
            int result = avformat_open_input(&input.context,
                                             paths[file_index].c_str(), nullptr,
                                             nullptr);
            if (result < 0) {
                (void)std::remove(output_path.c_str());
                return failure(ffmpeg_error("Could not open a selected video",
                                            result));
            }
            result = avformat_find_stream_info(input.context, nullptr);
            if (result < 0) {
                (void)std::remove(output_path.c_str());
                return failure(ffmpeg_error("Could not inspect a selected video",
                                            result));
            }

            std::vector<int> stream_map(input.context->nb_streams, -1);
            std::size_t video_count = 0;
            std::size_t audio_count = 0;
            if (file_index == 0) {
                result = avformat_alloc_output_context2(
                    &output.context, nullptr, "mp4", output_path.c_str());
                if (result < 0 || output.context == nullptr) {
                    return failure("Could not create merged MP4");
                }
                for (unsigned int index = 0; index < input.context->nb_streams;
                     ++index) {
                    AVStream *source = input.context->streams[index];
                    if (source->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                        source->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                        continue;
                    }
                    if ((source->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                         source->codecpar->codec_id != AV_CODEC_ID_H264) ||
                        (source->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                         source->codecpar->codec_id != AV_CODEC_ID_AAC)) {
                        (void)std::remove(output_path.c_str());
                        return failure(
                            "Selected videos must use H.264 video and AAC audio");
                    }
                    if (source->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        ++video_count;
                    } else {
                        ++audio_count;
                    }
                    AVStream *destination =
                        avformat_new_stream(output.context, nullptr);
                    if (destination == nullptr ||
                        avcodec_parameters_copy(destination->codecpar,
                                                source->codecpar) < 0) {
                        (void)std::remove(output_path.c_str());
                        return failure("Could not describe merged video streams");
                    }
                    destination->codecpar->codec_tag = 0;
                    destination->time_base = source->time_base;
                    stream_map[index] = destination->index;
                }
                if (video_count != 1 || audio_count > 1) {
                    (void)std::remove(output_path.c_str());
                    return failure("Selected videos have unsupported streams");
                }
                if ((output.context->oformat->flags & AVFMT_NOFILE) == 0) {
                    result = avio_open(&output.context->pb, output_path.c_str(),
                                       AVIO_FLAG_WRITE);
                    if (result < 0) {
                        (void)std::remove(output_path.c_str());
                        return failure(ffmpeg_error("Could not create merged MP4",
                                                    result));
                    }
                    output.file_open = true;
                }
                AVDictionary *options = nullptr;
                av_dict_set(&options, "movflags", "+faststart", 0);
                result = avformat_write_header(output.context, &options);
                av_dict_free(&options);
                if (result < 0) {
                    (void)std::remove(output_path.c_str());
                    return failure(ffmpeg_error("Could not start merged MP4",
                                                result));
                }
                header_written = true;
            } else {
                for (unsigned int input_index = 0;
                     input_index < input.context->nb_streams; ++input_index) {
                    AVStream *source = input.context->streams[input_index];
                    if (source->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                        source->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                        continue;
                    }
                    int matched = -1;
                    for (unsigned int output_index = 0;
                         output_index < output.context->nb_streams;
                         ++output_index) {
                        if (compatible_stream(
                                source->codecpar,
                                output.context->streams[output_index]->codecpar)) {
                            if (matched >= 0) {
                                matched = -1;
                                break;
                            }
                            matched = static_cast<int>(output_index);
                        }
                    }
                    if (matched < 0) {
                        (void)std::remove(output_path.c_str());
                        return failure("Selected videos use incompatible formats");
                    }
                    stream_map[input_index] = matched;
                    if (source->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        ++video_count;
                    } else {
                        ++audio_count;
                    }
                }
                if (video_count != 1 ||
                    audio_count != output.context->nb_streams - 1) {
                    (void)std::remove(output_path.c_str());
                    return failure("Selected videos use incompatible streams");
                }
            }

            std::vector<std::int64_t> first_timestamp(
                input.context->nb_streams, AV_NOPTS_VALUE);
            std::int64_t file_end_us = 0;
            while ((result = av_read_frame(input.context, packet.packet)) >= 0) {
                const int input_index = packet.packet->stream_index;
                if (input_index < 0 ||
                    static_cast<std::size_t>(input_index) >= stream_map.size() ||
                    stream_map[input_index] < 0) {
                    av_packet_unref(packet.packet);
                    continue;
                }
                AVStream *source = input.context->streams[input_index];
                AVStream *destination =
                    output.context->streams[stream_map[input_index]];
                std::int64_t timestamp = packet.packet->dts != AV_NOPTS_VALUE
                    ? packet.packet->dts : packet.packet->pts;
                if (first_timestamp[input_index] == AV_NOPTS_VALUE) {
                    first_timestamp[input_index] =
                        timestamp == AV_NOPTS_VALUE ? 0 : timestamp;
                }
                const std::int64_t origin = first_timestamp[input_index];
                if (packet.packet->pts != AV_NOPTS_VALUE) {
                    packet.packet->pts -= origin;
                }
                if (packet.packet->dts != AV_NOPTS_VALUE) {
                    packet.packet->dts -= origin;
                }
                av_packet_rescale_ts(packet.packet, source->time_base,
                                     destination->time_base);
                const std::int64_t stream_offset = av_rescale_q(
                    output_offset_us, AV_TIME_BASE_Q, destination->time_base);
                if (packet.packet->pts != AV_NOPTS_VALUE) {
                    packet.packet->pts += stream_offset;
                }
                if (packet.packet->dts != AV_NOPTS_VALUE) {
                    packet.packet->dts += stream_offset;
                }
                const std::int64_t packet_timestamp =
                    std::max(packet.packet->pts, packet.packet->dts);
                if (packet_timestamp != AV_NOPTS_VALUE) {
                    const std::int64_t packet_end = packet_timestamp +
                        std::max<std::int64_t>(packet.packet->duration, 0);
                    file_end_us = std::max(
                        file_end_us,
                        av_rescale_q(packet_end - stream_offset,
                                     destination->time_base, AV_TIME_BASE_Q));
                }
                processed_bytes += static_cast<std::uint64_t>(
                    std::max(packet.packet->size, 0));
                packet.packet->stream_index = destination->index;
                packet.packet->pos = -1;
                result = av_interleaved_write_frame(output.context,
                                                    packet.packet);
                av_packet_unref(packet.packet);
                if (result < 0) {
                    (void)std::remove(output_path.c_str());
                    return failure(ffmpeg_error("Could not write merged MP4",
                                                result));
                }
                if (progress &&
                    !progress(std::min(processed_bytes, total_bytes),
                              total_bytes)) {
                    (void)std::remove(output_path.c_str());
                    return failure("Transfer cancelled");
                }
            }
            if (result != AVERROR_EOF || file_end_us <= 0) {
                (void)std::remove(output_path.c_str());
                return failure("Could not read a complete selected video");
            }
            if (output_offset_us >
                std::numeric_limits<std::int64_t>::max() - file_end_us) {
                (void)std::remove(output_path.c_str());
                return failure("Merged video duration is too large");
            }
            output_offset_us += file_end_us;
        }

        if (!header_written || av_write_trailer(output.context) < 0) {
            (void)std::remove(output_path.c_str());
            return failure("Could not finish merged MP4");
        }
        if (output.file_open) {
            avio_closep(&output.context->pb);
            output.file_open = false;
        }

        struct stat status {};
        if (stat(output_path.c_str(), &status) != 0 || status.st_size <= 0 ||
            static_cast<std::uint64_t>(status.st_size) >
                kMaximumMergedVideoBytes) {
            (void)std::remove(output_path.c_str());
            return failure("Merged video exceeds the 48 MiB Telegram limit");
        }
        if (progress && !progress(total_bytes, total_bytes)) {
            (void)std::remove(output_path.c_str());
            return failure("Transfer cancelled");
        }

        MediaItem merged{output_path, "NXGallery-merged.mp4", MediaKind::Video,
                         static_cast<std::int64_t>(std::time(nullptr)),
                         static_cast<std::uint64_t>(status.st_size)};
        return {true, std::move(merged), {}};
    } catch (...) {
        (void)std::remove(output_path.c_str());
        return failure("Video merge failed");
    }
}

}  // namespace nxgallery
