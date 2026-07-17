#pragma once

#include <nxgallery/album_index.hpp>

#include <cstddef>
#include <string>

namespace nxgallery {

AlbumScanResult scan_horizon_album(std::size_t maximum_items = 5000) noexcept;
bool materialize_media_path(const MediaItem &media, std::string &path,
                            std::string &error) noexcept;
void shutdown_horizon_album() noexcept;

}  // namespace nxgallery
