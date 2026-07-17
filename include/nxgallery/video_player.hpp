#pragma once

#include <nxgallery/model.hpp>

#include <SDL2/SDL.h>

#include <memory>
#include <cstdint>
#include <string>

namespace nxgallery {

class VideoPlayer {
public:
    explicit VideoPlayer(SDL_Renderer *renderer);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    void play(const MediaItem &media);
    void toggle_pause();
    void stop();
    void update_texture();

    SDL_Texture *texture() const noexcept;
    bool active() const noexcept;
    bool paused() const noexcept;
    std::uint64_t frames_decoded() const noexcept;
    std::uint64_t position_ms() const noexcept;
    std::uint64_t duration_ms() const noexcept;
    std::string status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nxgallery
