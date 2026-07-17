#include <nxgallery/horizon_album.hpp>

#include <switch.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nxgallery {
namespace {

constexpr char kCapsPrefix[] = "caps:/";
constexpr char kCacheDirectory[] = "sdmc:/switch/nxgallery/cache-v2";
constexpr std::size_t kMaximumServiceEntries = 20000;
constexpr u64 kMaximumPhotoMaterialization = 32U * 1024U * 1024U;
constexpr u64 kMaximumVideoMaterialization = 50U * 1024U * 1024U;
constexpr u64 kMaximumThumbnailMaterialization = 512U * 1024U;
constexpr std::size_t kMovieReadBlock = 0x40000;

std::mutex g_album_mutex;
bool g_capsa_initialized{};
std::vector<CapsAlbumEntry> g_entries;

std::string result_code(Result result) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned int>(result));
    return text;
}

bool is_photo(const CapsAlbumEntry &entry) {
    return entry.file_id.content == CapsAlbumFileContents_ScreenShot ||
           entry.file_id.content == CapsAlbumFileContents_ExtraScreenShot;
}

bool is_video(const CapsAlbumEntry &entry) {
    return entry.file_id.content == CapsAlbumFileContents_Movie ||
           entry.file_id.content == CapsAlbumFileContents_ExtraMovie;
}

std::int64_t sortable_datetime(const CapsAlbumFileDateTime &value) {
    return static_cast<std::int64_t>(value.year) * 10000000000000LL +
           static_cast<std::int64_t>(value.month) * 100000000000LL +
           static_cast<std::int64_t>(value.day) * 1000000000LL +
           static_cast<std::int64_t>(value.hour) * 10000000LL +
           static_cast<std::int64_t>(value.minute) * 100000LL +
           static_cast<std::int64_t>(value.second) * 1000LL + value.id;
}

bool newer(const CapsAlbumEntry &left, const CapsAlbumEntry &right) {
    const std::int64_t left_time = sortable_datetime(left.file_id.datetime);
    const std::int64_t right_time = sortable_datetime(right.file_id.datetime);
    if (left_time != right_time) return left_time > right_time;
    if (left.file_id.application_id != right.file_id.application_id) {
        return left.file_id.application_id > right.file_id.application_id;
    }
    return left.file_id.storage > right.file_id.storage;
}

std::string display_filename(const CapsAlbumEntry &entry) {
    const auto &date = entry.file_id.datetime;
    char text[96]{};
    std::snprintf(text, sizeof(text), "%04u%02u%02u%02u%02u%02u%02u-%016llx.%s",
                  date.year, date.month, date.day, date.hour, date.minute,
                  date.second, date.id,
                  static_cast<unsigned long long>(entry.file_id.application_id),
                  is_photo(entry) ? "jpg" : "mp4");
    return text;
}

std::string cached_path(const CapsAlbumEntry &entry) {
    return std::string(kCacheDirectory) + "/" + display_filename(entry);
}

std::string cached_thumbnail_path(const CapsAlbumEntry &entry) {
    return std::string(kCacheDirectory) + "/" + display_filename(entry) + ".thumbnail.jpg";
}

bool parse_caps_index(const std::string &path, std::size_t &index) {
    if (path.rfind(kCapsPrefix, 0) != 0) return false;
    const char *text = path.c_str() + sizeof(kCapsPrefix) - 1;
    if (*text == '\0') return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value > std::numeric_limits<std::size_t>::max()) return false;
    index = static_cast<std::size_t>(value);
    return true;
}

bool ensure_cache_directory(std::string &error) {
    if (mkdir("sdmc:/switch/nxgallery", 0777) != 0 && errno != EEXIST) {
        error = "Could not create NX Gallery app directory";
        return false;
    }
    if (mkdir(kCacheDirectory, 0777) != 0 && errno != EEXIST) {
        error = "Could not create NX Gallery media cache";
        return false;
    }
    return true;
}

bool existing_file(const std::string &path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_size > 0;
}

bool materialize_photo(const CapsAlbumEntry &entry, const std::string &path,
                       std::string &error) {
    u64 size = 0;
    Result result = capsaGetAlbumFileSize(&entry.file_id, &size);
    if (R_FAILED(result)) {
        error = "Could not read photo size: " + result_code(result);
        return false;
    }
    if (size == 0 || size > kMaximumPhotoMaterialization) {
        error = "Photo capture has an unsupported size";
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    u64 actual_size = 0;
    result = capsaLoadAlbumFile(&entry.file_id, &actual_size, bytes.data(), size);
    if (R_FAILED(result) || actual_size == 0 || actual_size > size) {
        error = "Could not load photo capture: " + result_code(result);
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(actual_size));
    output.close();
    if (!output) {
        std::remove(path.c_str());
        error = "Could not cache photo capture";
        return false;
    }
    return true;
}

bool materialize_thumbnail(const CapsAlbumEntry &entry, const std::string &path,
                           std::string &error) {
    std::vector<unsigned char> bytes(kMaximumThumbnailMaterialization);
    u64 actual_size = 0;
    const Result result = capsaLoadAlbumFileThumbnail(
        &entry.file_id, &actual_size, bytes.data(), bytes.size());
    if (R_FAILED(result) || actual_size == 0 || actual_size > bytes.size()) {
        error = "Could not load capture thumbnail: " + result_code(result);
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(actual_size));
    output.close();
    if (!output) {
        std::remove(path.c_str());
        error = "Could not cache capture thumbnail";
        return false;
    }
    return true;
}

bool materialize_video(const CapsAlbumEntry &entry, const std::string &path,
                       std::string &error) {
    u64 stream = 0;
    Result result = capsaOpenAlbumMovieStream(&stream, &entry.file_id);
    if (R_FAILED(result)) {
        error = "Could not open movie capture: " + result_code(result);
        return false;
    }
    u64 size = 0;
    result = capsaGetAlbumMovieStreamSize(stream, &size);
    if (R_FAILED(result) || size == 0 || size > kMaximumVideoMaterialization) {
        (void)capsaCloseAlbumMovieStream(stream);
        error = R_FAILED(result) ? "Could not read movie size: " + result_code(result) :
                                  "Video exceeds the Bot API 50 MB limit";
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        (void)capsaCloseAlbumMovieStream(stream);
        error = "Could not create cached movie capture";
        return false;
    }
    std::vector<unsigned char> buffer(kMovieReadBlock);
    u64 offset = 0;
    while (offset < size) {
        u64 actual_size = 0;
        result = capsaReadMovieDataFromAlbumMovieReadStream(
            stream, static_cast<s64>(offset), buffer.data(), buffer.size(), &actual_size);
        if (R_FAILED(result) || actual_size == 0) break;
        const u64 remaining = size - offset;
        const u64 write_size = std::min<u64>(remaining, actual_size);
        output.write(reinterpret_cast<const char *>(buffer.data()),
                     static_cast<std::streamsize>(write_size));
        if (!output) break;
        offset += write_size;
    }
    const Result close_result = capsaCloseAlbumMovieStream(stream);
    output.close();
    if (R_FAILED(result) || R_FAILED(close_result) || !output || offset != size) {
        std::remove(path.c_str());
        error = R_FAILED(result) ? "Could not read movie capture: " + result_code(result) :
                                  "Could not cache complete movie capture";
        return false;
    }
    return true;
}

}  // namespace

AlbumScanResult scan_horizon_album(std::size_t maximum_items) noexcept {
    AlbumScanResult output;
    if (maximum_items == 0) {
        output.error = "Album item limit must be non-zero";
        return output;
    }
    try {
        std::lock_guard<std::mutex> lock(g_album_mutex);
        if (!g_capsa_initialized) {
            const Result result = capsaInitialize();
            if (R_FAILED(result)) {
                output.error = "Horizon Album service unavailable: " + result_code(result);
                return output;
            }
            g_capsa_initialized = true;
        }
        g_entries.clear();
        for (const CapsAlbumStorage storage : {CapsAlbumStorage_Nand, CapsAlbumStorage_Sd}) {
            u64 count = 0;
            Result result = capsaGetAlbumFileCount(storage, &count);
            if (R_FAILED(result)) continue;
            count = std::min<u64>(count, kMaximumServiceEntries);
            std::vector<CapsAlbumEntry> entries(static_cast<std::size_t>(count));
            u64 actual_count = 0;
            if (count > 0) {
                result = capsaGetAlbumFileList(storage, &actual_count, entries.data(), count);
                if (R_FAILED(result)) continue;
            }
            entries.resize(static_cast<std::size_t>(std::min(actual_count, count)));
            for (auto &entry : entries) {
                if (is_photo(entry) || is_video(entry)) g_entries.push_back(std::move(entry));
            }
        }
        std::sort(g_entries.begin(), g_entries.end(), newer);
        if (g_entries.size() > maximum_items) {
            g_entries.resize(maximum_items);
            output.truncated = true;
        }
        output.items.reserve(g_entries.size());
        for (std::size_t index = 0; index < g_entries.size(); ++index) {
            const CapsAlbumEntry &entry = g_entries[index];
            output.items.push_back({std::string(kCapsPrefix) + std::to_string(index),
                                    display_filename(entry),
                                    is_photo(entry) ? MediaKind::Photo : MediaKind::Video,
                                    sortable_datetime(entry.file_id.datetime), entry.size});
        }
    } catch (...) {
        output.items.clear();
        output.error = "Horizon Album enumeration ran out of memory";
    }
    return output;
}

bool materialize_media_path(const MediaItem &media, std::string &path,
                            std::string &error) noexcept {
    try {
        std::size_t index = 0;
        if (!parse_caps_index(media.path, index)) {
            path = media.path;
            return true;
        }
        std::lock_guard<std::mutex> lock(g_album_mutex);
        if (!g_capsa_initialized || index >= g_entries.size()) {
            error = "Capture is no longer available from Horizon Album";
            return false;
        }
        const CapsAlbumEntry &entry = g_entries[index];
        path = cached_path(entry);
        if (existing_file(path)) return true;
        if (!ensure_cache_directory(error)) return false;
        return is_photo(entry) ? materialize_photo(entry, path, error) :
                                 materialize_video(entry, path, error);
    } catch (...) {
        error = "Capture materialization ran out of memory";
        return false;
    }
}

bool materialize_thumbnail_path(const MediaItem &media, std::string &path,
                                std::string &error) noexcept {
    try {
        std::size_t index = 0;
        if (!parse_caps_index(media.path, index)) {
            if (media.kind == MediaKind::Photo) {
                path = media.path;
                return true;
            }
            error = "Video thumbnail is unavailable";
            return false;
        }
        std::lock_guard<std::mutex> lock(g_album_mutex);
        if (!g_capsa_initialized || index >= g_entries.size()) {
            error = "Capture is no longer available from Horizon Album";
            return false;
        }
        const CapsAlbumEntry &entry = g_entries[index];
        path = cached_thumbnail_path(entry);
        if (existing_file(path)) return true;
        if (!ensure_cache_directory(error)) return false;
        return materialize_thumbnail(entry, path, error);
    } catch (...) {
        error = "Thumbnail materialization ran out of memory";
        return false;
    }
}

void shutdown_horizon_album() noexcept {
    std::lock_guard<std::mutex> lock(g_album_mutex);
    g_entries.clear();
    if (g_capsa_initialized) {
        capsaExit();
        g_capsa_initialized = false;
    }
}

}  // namespace nxgallery
