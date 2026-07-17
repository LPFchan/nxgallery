#include <nxgallery/album_index.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nxgallery {
namespace {

std::string lowercase_extension(const std::string &path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

bool classify(const std::string &path, MediaKind &kind) {
    const std::string extension = lowercase_extension(path);
    if (extension == ".jpg" || extension == ".jpeg" || extension == ".png") {
        kind = MediaKind::Photo;
        return true;
    }
    if (extension == ".mp4") {
        kind = MediaKind::Video;
        return true;
    }
    return false;
}

bool newer(const MediaItem &left, const MediaItem &right) {
    if (left.modified_time != right.modified_time) return left.modified_time > right.modified_time;
    return left.path > right.path;
}

std::string basename(const std::string &path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool scan_directory(const std::string &path, std::size_t maximum,
                    std::vector<MediaItem> &items, bool &truncated,
                    std::string &error) {
    DIR *directory = opendir(path.c_str());
    if (directory == nullptr) {
        error = "Could not open album directory: " + std::string(std::strerror(errno));
        return false;
    }
    while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' ||
            (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) continue;
        const std::string child = path + (path.empty() || path.back() == '/' ? "" : "/") + entry->d_name;
        struct stat status {};
        if (stat(child.c_str(), &status) != 0) continue;
        if (S_ISDIR(status.st_mode)) {
            if (!scan_directory(child, maximum, items, truncated, error)) {
                closedir(directory);
                return false;
            }
            continue;
        }
        if (!S_ISREG(status.st_mode)) continue;
        MediaKind kind;
        if (!classify(child, kind)) continue;
        MediaItem candidate{child, basename(child), kind,
                            static_cast<std::int64_t>(status.st_mtime),
                            status.st_size < 0 ? 0U : static_cast<std::uint64_t>(status.st_size)};
        if (items.size() < maximum) {
            items.push_back(std::move(candidate));
            std::push_heap(items.begin(), items.end(), newer);
        } else {
            truncated = true;
            if (newer(candidate, items.front())) {
                std::pop_heap(items.begin(), items.end(), newer);
                items.back() = std::move(candidate);
                std::push_heap(items.begin(), items.end(), newer);
            }
        }
    }
    closedir(directory);
    return true;
}

}  // namespace

AlbumScanResult scan_album(const std::string &root, std::size_t maximum_items) noexcept {
    AlbumScanResult result;
    if (root.empty() || maximum_items == 0) {
        result.error = "Album path and item limit must be non-empty";
        return result;
    }
    try {
        if (!scan_directory(root, maximum_items, result.items, result.truncated, result.error)) return result;
        std::sort(result.items.begin(), result.items.end(), newer);
    } catch (...) {
        result.items.clear();
        result.error = "Album scan ran out of memory";
    }
    return result;
}

}  // namespace nxgallery
