#include <nxgallery/https_trust.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

#ifndef NXGALLERY_HOST_TEST
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace nxgallery {
namespace {

#ifndef NXGALLERY_HOST_TEST
bool load_with_openssl(const char *path) noexcept {
    ERR_clear_error();
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr) return false;
    const bool loaded = SSL_CTX_load_verify_file(context, path) == 1;
    SSL_CTX_free(context);
    ERR_clear_error();
    return loaded;
}
#endif

}  // namespace

HttpsTrustStatus preflight_https_ca_file(const char *path, HttpsCaLoader loader) noexcept {
    if (path == nullptr || *path == '\0' || loader == nullptr) return HttpsTrustStatus::Invalid;
    struct stat status {};
    if (stat(path, &status) != 0) return errno == ENOENT ? HttpsTrustStatus::Missing : HttpsTrustStatus::Unreadable;
    if (!S_ISREG(status.st_mode)) return HttpsTrustStatus::NotRegularFile;
    if (status.st_size == 0) return HttpsTrustStatus::Empty;
    if (status.st_size < 0 || static_cast<unsigned long long>(status.st_size) > kHttpsCaFileMaximumBytes) return HttpsTrustStatus::TooLarge;
    std::FILE *input = std::fopen(path, "rb");
    if (input == nullptr) return HttpsTrustStatus::Unreadable;
    std::array<unsigned char, 4096> buffer{};
    std::size_t total = 0;
    while (total < static_cast<std::size_t>(status.st_size)) {
        const std::size_t remaining = static_cast<std::size_t>(status.st_size) - total;
        const std::size_t requested = remaining < buffer.size() ? remaining : buffer.size();
        const std::size_t read = std::fread(buffer.data(), 1, requested, input);
        total += read;
        if (read != requested) break;
    }
    const bool readable = total == static_cast<std::size_t>(status.st_size) && std::ferror(input) == 0;
    std::fclose(input);
    if (!readable) return HttpsTrustStatus::Unreadable;
    return loader(path) ? HttpsTrustStatus::Available : HttpsTrustStatus::Invalid;
}

#ifndef NXGALLERY_HOST_TEST
HttpsTrustStatus preflight_https_ca_file() noexcept { return preflight_https_ca_file(kHttpsCaFile, load_with_openssl); }
#endif

const char *https_trust_diagnostic(HttpsTrustStatus status) noexcept {
    switch (status) {
        case HttpsTrustStatus::Available: return "";
        case HttpsTrustStatus::Missing: return "Telegram unavailable: CA bundle is missing";
        case HttpsTrustStatus::NotRegularFile: return "Telegram unavailable: CA bundle path is not a file";
        case HttpsTrustStatus::Empty: return "Telegram unavailable: CA bundle is empty";
        case HttpsTrustStatus::TooLarge: return "Telegram unavailable: CA bundle is too large";
        case HttpsTrustStatus::Unreadable: return "Telegram unavailable: CA bundle cannot be read";
        case HttpsTrustStatus::Invalid: return "Telegram unavailable: CA bundle is invalid";
    }
    return "Telegram HTTPS preflight failed";
}

}  // namespace nxgallery
