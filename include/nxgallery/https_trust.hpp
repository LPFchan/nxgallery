#pragma once

#include <cstddef>

namespace nxgallery {

constexpr char kHttpsCaFile[] = "sdmc:/switch/nxgallery/openssl/cert.pem";
constexpr std::size_t kHttpsCaFileMaximumBytes = 512U * 1024U;

enum class HttpsTrustStatus { Available, Missing, NotRegularFile, Empty, TooLarge, Unreadable, Invalid };
using HttpsCaLoader = bool (*)(const char *path) noexcept;

HttpsTrustStatus preflight_https_ca_file(const char *path, HttpsCaLoader loader) noexcept;
bool ensure_https_ca_file() noexcept;
HttpsTrustStatus preflight_https_ca_file() noexcept;
const char *https_trust_diagnostic(HttpsTrustStatus status) noexcept;

}  // namespace nxgallery
