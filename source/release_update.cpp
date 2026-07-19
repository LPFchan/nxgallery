#include <nxgallery/https_trust.hpp>
#include <nxgallery/release_update.hpp>

#include <curl/curl.h>
#include <json-c/json.h>
#include <openssl/evp.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace nxgallery {
namespace {

constexpr std::size_t kMaximumReleaseResponseBytes = 1024U * 1024U;
constexpr std::size_t kMaximumNroBytes = 128U * 1024U * 1024U;
constexpr char kDownloadUrlPrefix[] =
    "https://github.com/LPFchan/nxgallery/releases/download/";

struct ReleaseAsset {
    std::string version;
    std::string url;
    std::string sha256;
    std::uint64_t size{};
};

struct JsonDeleter {
    void operator()(json_object *object) const noexcept {
        if (object != nullptr) json_object_put(object);
    }
};

bool json_string(json_object *object, const char *key, std::string &value) {
    json_object *field = nullptr;
    if (!json_object_object_get_ex(object, key, &field) || field == nullptr ||
        json_object_get_type(field) != json_type_string) {
        return false;
    }
    value = json_object_get_string(field);
    return true;
}

bool parse_release(const std::string &body, ReleaseAsset &release,
                   std::string &error) {
    json_tokener *tokener = json_tokener_new();
    if (tokener == nullptr) {
        error = "Could not prepare the GitHub release response parser";
        return false;
    }
    json_object *parsed = json_tokener_parse_ex(
        tokener, body.data(), static_cast<int>(body.size()));
    const json_tokener_error parse_error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    std::unique_ptr<json_object, JsonDeleter> root(parsed);
    if (parse_error != json_tokener_success || root == nullptr ||
        json_object_get_type(root.get()) != json_type_object) {
        error = "GitHub returned an invalid release response";
        return false;
    }

    if (!json_string(root.get(), "tag_name", release.version)) {
        error = "The latest GitHub release has no version tag";
        return false;
    }
    json_object *prerelease = nullptr;
    if (json_object_object_get_ex(root.get(), "prerelease", &prerelease) &&
        json_object_get_boolean(prerelease)) {
        error = "The latest GitHub release is a prerelease";
        return false;
    }
    json_object *assets = nullptr;
    if (!json_object_object_get_ex(root.get(), "assets", &assets) ||
        assets == nullptr || json_object_get_type(assets) != json_type_array) {
        error = "The latest GitHub release has no assets";
        return false;
    }
    const std::size_t count = json_object_array_length(assets);
    for (std::size_t index = 0; index < count; ++index) {
        json_object *asset = json_object_array_get_idx(assets, index);
        if (asset == nullptr || json_object_get_type(asset) != json_type_object) {
            continue;
        }
        std::string name;
        if (!json_string(asset, "name", name) || name != kReleaseAssetName) {
            continue;
        }
        std::string digest;
        json_object *size = nullptr;
        if (!json_string(asset, "browser_download_url", release.url) ||
            !json_string(asset, "digest", digest) ||
            !json_object_object_get_ex(asset, "size", &size) || size == nullptr ||
            json_object_get_type(size) != json_type_int) {
            error = "The nxgallery.nro release asset is incomplete";
            return false;
        }
        constexpr char kSha256Prefix[] = "sha256:";
        if (digest.rfind(kSha256Prefix, 0) != 0 ||
            digest.size() != sizeof(kSha256Prefix) - 1 + 64) {
            error = "The nxgallery.nro release asset has no SHA-256 digest";
            return false;
        }
        const std::int64_t signed_size = json_object_get_int64(size);
        if (signed_size <= 0 ||
            static_cast<std::uint64_t>(signed_size) > kMaximumNroBytes) {
            error = "The nxgallery.nro release asset has an invalid size";
            return false;
        }
        release.size = static_cast<std::uint64_t>(signed_size);
        release.sha256 = digest.substr(sizeof(kSha256Prefix) - 1);
        if (release.url.rfind(kDownloadUrlPrefix, 0) != 0) {
            error = "The nxgallery.nro release asset has an unexpected URL";
            return false;
        }
        return true;
    }
    error = "The latest GitHub release does not include nxgallery.nro";
    return false;
}

struct MemoryDownload {
    std::string bytes;
    std::size_t maximum{};
};

std::size_t append_limited(void *contents, std::size_t size,
                           std::size_t count, void *context) {
    const std::size_t bytes = size * count;
    auto &download = *static_cast<MemoryDownload *>(context);
    if (size != 0 && bytes / size != count) return 0;
    if (bytes > download.maximum - std::min(download.maximum,
                                             download.bytes.size())) {
        return 0;
    }
    download.bytes.append(static_cast<const char *>(contents), bytes);
    return bytes;
}

struct FileDownload {
    std::FILE *output{};
    std::uint64_t written{};
    std::uint64_t maximum{};
};

std::size_t write_limited(void *contents, std::size_t size,
                          std::size_t count, void *context) {
    const std::size_t bytes = size * count;
    auto &download = *static_cast<FileDownload *>(context);
    if (size != 0 && bytes / size != count) return 0;
    if (bytes > download.maximum -
                    std::min(download.maximum, download.written)) {
        return 0;
    }
    const std::size_t written = std::fwrite(contents, 1, bytes, download.output);
    download.written += written;
    return written;
}

int cancel_transfer(void *context, curl_off_t, curl_off_t,
                    curl_off_t, curl_off_t) {
    auto *cancel = static_cast<std::atomic<bool> *>(context);
    return cancel != nullptr && cancel->load() ? 1 : 0;
}

bool configure_https(CURL *curl, const std::string &url,
                     std::atomic<bool> *cancel_requested) {
    if (curl == nullptr) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NX-Gallery-Updater/1");
    curl_easy_setopt(curl, CURLOPT_CAINFO, kHttpsCaFile);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 180000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (cancel_requested != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_transfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel_requested);
    }
    return true;
}

bool fetch_latest_release(std::string &body, long &response_code,
                          std::atomic<bool> *cancel_requested,
                          std::string &error) {
    CURL *curl = curl_easy_init();
    if (!configure_https(curl, kGithubLatestReleaseUrl, cancel_requested)) {
        error = "Could not prepare the GitHub release check";
        return false;
    }
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    MemoryDownload download{{}, kMaximumReleaseResponseBytes};
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_limited);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        error = "GitHub release check failed: " +
            std::string(curl_easy_strerror(code));
        return false;
    }
    body = std::move(download.bytes);
    return true;
}

bool download_asset(const ReleaseAsset &release, const std::string &path,
                    std::atomic<bool> *cancel_requested, std::string &error) {
    std::FILE *output = std::fopen(path.c_str(), "wb");
    if (output == nullptr) {
        error = "Could not create the update file on the SD card";
        return false;
    }
    CURL *curl = curl_easy_init();
    if (!configure_https(curl, release.url, cancel_requested)) {
        std::fclose(output);
        std::remove(path.c_str());
        error = "Could not prepare the update download";
        return false;
    }
    FileDownload download{output, 0, release.size};
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_limited);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    const CURLcode code = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);
    const bool flushed = std::fflush(output) == 0;
    errno = 0;
    const int sync_result = flushed ? fsync(fileno(output)) : -1;
    const bool synced = flushed &&
        (sync_result == 0 || errno == ENOSYS || errno == EINVAL);
    const bool file_ok = std::ferror(output) == 0;
    std::fclose(output);
    if (code != CURLE_OK || response_code != 200 || !file_ok || !synced ||
        download.written != release.size) {
        std::remove(path.c_str());
        error = code == CURLE_ABORTED_BY_CALLBACK ? "Update cancelled" :
            "The nxgallery.nro download was incomplete";
        return false;
    }
    return true;
}

bool sha256_file(const std::string &path, std::string &digest) {
    std::FILE *input = std::fopen(path.c_str(), "rb");
    if (input == nullptr) return false;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        std::fclose(input);
        return false;
    }
    std::array<unsigned char, 64U * 1024U> buffer{};
    bool ok = true;
    while (true) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
        if (read > 0 && EVP_DigestUpdate(context, buffer.data(), read) != 1) {
            ok = false;
            break;
        }
        if (read < buffer.size()) {
            if (std::ferror(input) != 0) ok = false;
            break;
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    ok = ok && EVP_DigestFinal_ex(context, bytes.data(), &length) == 1 &&
        length == 32;
    EVP_MD_CTX_free(context);
    std::fclose(input);
    if (!ok) return false;
    constexpr char kHex[] = "0123456789abcdef";
    digest.clear();
    digest.reserve(length * 2);
    for (unsigned int index = 0; index < length; ++index) {
        digest.push_back(kHex[bytes[index] >> 4]);
        digest.push_back(kHex[bytes[index] & 0x0f]);
    }
    return true;
}

std::uint32_t read_u32_le(const unsigned char *bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool valid_nro(const std::string &path, std::uint64_t expected_size) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 184 ||
        static_cast<std::uint64_t>(status.st_size) != expected_size) {
        return false;
    }
    std::FILE *input = std::fopen(path.c_str(), "rb");
    if (input == nullptr) return false;
    std::array<unsigned char, 28> header{};
    const bool header_read =
        std::fread(header.data(), 1, header.size(), input) == header.size();
    const std::uint32_t nro_size = header_read ? read_u32_le(header.data() + 24) : 0;
    bool valid = header_read && std::memcmp(header.data() + 16, "NRO0", 4) == 0 &&
        nro_size >= 128 && nro_size <= expected_size - 56 &&
        std::fseek(input, static_cast<long>(nro_size), SEEK_SET) == 0;
    std::array<char, 4> aset{};
    valid = valid && std::fread(aset.data(), 1, aset.size(), input) == aset.size() &&
        std::memcmp(aset.data(), "ASET", 4) == 0;
    std::fclose(input);
    return valid;
}

bool install_atomically(const std::string &temporary,
                        const std::string &installed, std::string &error) {
    const std::string previous = installed + ".previous";
    (void)std::remove(previous.c_str());
    if (std::rename(installed.c_str(), previous.c_str()) != 0 && errno != ENOENT) {
        error = "Could not preserve the previous NX Gallery version";
        return false;
    }
    if (std::rename(temporary.c_str(), installed.c_str()) == 0) return true;
    const int install_error = errno;
    (void)std::rename(previous.c_str(), installed.c_str());
    errno = install_error;
    error = "Could not install the downloaded NX Gallery update";
    return false;
}

}  // namespace

UpdateResult check_latest_release(const std::string &current_version,
                                  std::atomic<bool> *cancel_requested) {
    std::string response;
    std::string error;
    long response_code = 0;
    if (!fetch_latest_release(response, response_code, cancel_requested, error)) {
        return {UpdateOutcome::Failed, {}, std::move(error)};
    }
    if (response_code == 404) return {};
    if (response_code != 200) {
        return {UpdateOutcome::Failed, {},
                "GitHub release check returned HTTP " +
                    std::to_string(response_code)};
    }
    ReleaseAsset release;
    if (!parse_release(response, release, error)) {
        return {UpdateOutcome::Failed, release.version, std::move(error)};
    }
    if (!is_newer_release(current_version, release.version)) return {};

    UpdateResult result;
    result.outcome = UpdateOutcome::Available;
    result.version = std::move(release.version);
    result.message = "NX Gallery " + result.version + " is available";
    result.asset_url = std::move(release.url);
    result.sha256 = std::move(release.sha256);
    result.asset_size = release.size;
    return result;
}

UpdateResult install_release(const UpdateResult &available_release,
                             const std::string &installed_path,
                             std::atomic<bool> *cancel_requested) {
    if (available_release.outcome != UpdateOutcome::Available ||
        available_release.version.empty() || available_release.asset_url.empty() ||
        available_release.sha256.size() != 64 ||
        available_release.asset_size == 0) {
        return {UpdateOutcome::Failed, available_release.version,
                "The selected NX Gallery release is invalid"};
    }
    ReleaseAsset release{available_release.version, available_release.asset_url,
                         available_release.sha256,
                         available_release.asset_size};
    std::string error;

    const std::string temporary = installed_path + ".update";
    (void)std::remove(temporary.c_str());
    if (!download_asset(release, temporary, cancel_requested, error)) {
        return {UpdateOutcome::Failed, release.version, std::move(error)};
    }
    std::string actual_digest;
    if (!sha256_file(temporary, actual_digest) ||
        actual_digest != release.sha256 ||
        !valid_nro(temporary, release.size)) {
        std::remove(temporary.c_str());
        return {UpdateOutcome::Failed, release.version,
                "The downloaded nxgallery.nro failed verification"};
    }
    if (!install_atomically(temporary, installed_path, error)) {
        std::remove(temporary.c_str());
        return {UpdateOutcome::Failed, release.version, std::move(error)};
    }
    return {UpdateOutcome::Installed, release.version,
            "NX Gallery " + release.version +
                " is installed. Restart the app to use it."};
}

}  // namespace nxgallery
