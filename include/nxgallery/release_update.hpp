#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace nxgallery {

constexpr char kGithubLatestReleaseUrl[] =
    "https://api.github.com/repos/LPFchan/nxgallery/releases/latest";
constexpr char kReleaseAssetName[] = "nxgallery.nro";
constexpr char kInstalledNroPath[] =
    "sdmc:/switch/nxgallery/nxgallery.nro";

enum class UpdateOutcome { NoUpdate, Available, Installed, Failed };

struct UpdateResult {
    UpdateOutcome outcome{UpdateOutcome::NoUpdate};
    std::string version;
    std::string message;
    std::string asset_url;
    std::string sha256;
    std::uint64_t asset_size{};
};

// Accepts MAJOR.MINOR.PATCH with an optional leading v. Release suffixes are
// rejected so a stable build never silently installs a prerelease.
bool is_newer_release(const std::string &current,
                      const std::string &candidate) noexcept;

UpdateResult check_latest_release(
    const std::string &current_version,
    std::atomic<bool> *cancel_requested = nullptr);

UpdateResult install_release(
    const UpdateResult &available_release,
    const std::string &installed_path = kInstalledNroPath,
    std::atomic<bool> *cancel_requested = nullptr);

}  // namespace nxgallery
