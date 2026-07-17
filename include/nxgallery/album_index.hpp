#pragma once

#include <nxgallery/model.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace nxgallery {

struct AlbumScanResult {
    std::vector<MediaItem> items;
    std::string error;
    bool truncated{};

    explicit operator bool() const noexcept { return error.empty(); }
};

AlbumScanResult scan_album(const std::string &root, std::size_t maximum_items = 5000) noexcept;

}  // namespace nxgallery
