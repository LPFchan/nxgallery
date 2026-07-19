#include <nxgallery/album_index.hpp>
#include <nxgallery/crash_diagnostics.hpp>
#include <nxgallery/gallery_app.hpp>
#include <nxgallery/horizon_album.hpp>
#include <nxgallery/https_trust.hpp>
#include <nxgallery/telegram_config.hpp>
#include <nxgallery/video_player.hpp>

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pu/Plutonium>
#include <switch.h>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

// Plutonium's SDL renderer needs substantially more than libnx's default heap.
u64 __nx_heap_size = 256ULL * 1024ULL * 1024ULL;

namespace {
constexpr char kTelegramConfigPath[] = "sdmc:/switch/nxgallery/telegram-bot.conf";
constexpr char kRawAlbumPath[] = "sdmc:/Nintendo/Album";

struct ProbeOptions {
    bool enabled{};
    bool send_media{};
    std::string config_url;
    std::string config_pin_hex;
};

ProbeOptions parse_probe_options(int argc, char **argv) {
    ProbeOptions options;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) continue;
        const std::string argument(argv[index]);
        if (argument == "--probe") options.enabled = true;
        else if (argument == "--probe-send-media") options.send_media = true;
        else if (argument.rfind("--probe-config-url=", 0) == 0) {
            options.config_url = argument.substr(19);
        } else if (argument.rfind("--probe-config-pin-hex=", 0) == 0) {
            options.config_pin_hex = argument.substr(23);
        }
    }
    return options;
}

bool fetch_probe_config(const ProbeOptions &options, std::string &contents,
                        std::string &error) {
    constexpr char kHttpsPrefix[] = "https://";
    if (options.config_url.rfind(kHttpsPrefix, 0) != 0 ||
        options.config_pin_hex.size() != SHA256_DIGEST_LENGTH * 2) {
        error = "Probe configuration channel is unavailable";
        return false;
    }
    const std::size_t host_begin = sizeof(kHttpsPrefix) - 1;
    const std::size_t port_separator = options.config_url.find(':', host_begin);
    const std::size_t path_begin = options.config_url.find('/', host_begin);
    if (port_separator == std::string::npos || path_begin == std::string::npos ||
        port_separator >= path_begin) {
        error = "Probe configuration URL is malformed";
        return false;
    }
    const std::string host = options.config_url.substr(host_begin, port_separator - host_begin);
    const std::string port_text = options.config_url.substr(
        port_separator + 1, path_begin - port_separator - 1);
    char *port_end = nullptr;
    const unsigned long parsed_port = std::strtoul(port_text.c_str(), &port_end, 10);
    if (port_end == port_text.c_str() || *port_end != '\0' || parsed_port > 65535) {
        error = "Probe configuration port is malformed";
        return false;
    }

    std::array<unsigned char, SHA256_DIGEST_LENGTH> expected_digest{};
    for (std::size_t index = 0; index < expected_digest.size(); ++index) {
        const std::string byte = options.config_pin_hex.substr(index * 2, 2);
        char *end = nullptr;
        const unsigned long value = std::strtoul(byte.c_str(), &end, 16);
        if (end == byte.c_str() || *end != '\0' || value > 0xff) {
            error = "Probe configuration pin is malformed";
            return false;
        }
        expected_digest[index] = static_cast<unsigned char>(value);
    }

    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        error = "Could not create probe configuration socket";
        return false;
    }
    const timeval timeout{15, 0};
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(parsed_port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
        connect(socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(socket_fd);
        error = "Could not connect to probe configuration server";
        return false;
    }
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    SSL *tls = context == nullptr ? nullptr : SSL_new(context);
    if (tls == nullptr) {
        if (context != nullptr) SSL_CTX_free(context);
        close(socket_fd);
        error = "Could not create probe TLS session";
        return false;
    }
    SSL_set_verify(tls, SSL_VERIFY_NONE, nullptr);
    SSL_set_fd(tls, socket_fd);
    bool success = false;
    if (SSL_connect(tls) != 1) {
        error = "Probe TLS handshake failed";
    } else {
        X509 *certificate = SSL_get1_peer_certificate(tls);
        EVP_PKEY *public_key = certificate == nullptr ? nullptr : X509_get_pubkey(certificate);
        const int key_size = public_key == nullptr ? -1 : i2d_PUBKEY(public_key, nullptr);
        std::vector<unsigned char> key_der(key_size > 0 ? static_cast<std::size_t>(key_size) : 0);
        unsigned char *key_output = key_der.data();
        if (key_size > 0) (void)i2d_PUBKEY(public_key, &key_output);
        std::array<unsigned char, SHA256_DIGEST_LENGTH> actual_digest{};
        if (certificate == nullptr || public_key == nullptr || key_size <= 0 ||
            SHA256(key_der.data(), key_der.size(), actual_digest.data()) == nullptr ||
            CRYPTO_memcmp(actual_digest.data(), expected_digest.data(), actual_digest.size()) != 0) {
            error = "Probe TLS server identity did not match";
        } else {
            const std::string request = "GET " + options.config_url.substr(path_begin) +
                " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
            std::string response;
            if (SSL_write(tls, request.data(), static_cast<int>(request.size())) <= 0) {
                error = "Probe configuration request failed";
            } else {
                std::array<char, 4096> buffer{};
                int bytes = 0;
                while ((bytes = SSL_read(tls, buffer.data(), buffer.size())) > 0 &&
                       response.size() + static_cast<std::size_t>(bytes) <= 40U * 1024U) {
                    response.append(buffer.data(), static_cast<std::size_t>(bytes));
                }
                const std::size_t header_end = response.find("\r\n\r\n");
                if (response.rfind("HTTP/1.0 200", 0) != 0 &&
                    response.rfind("HTTP/1.1 200", 0) != 0) {
                    error = "Probe configuration server returned an error";
                } else if (header_end == std::string::npos || header_end + 4 >= response.size()) {
                    error = "Probe configuration response was empty";
                } else {
                    contents = response.substr(header_end + 4);
                    success = contents.size() <= 32U * 1024U;
                    if (!success) error = "Probe configuration response was too large";
                }
            }
        }
        if (public_key != nullptr) EVP_PKEY_free(public_key);
        if (certificate != nullptr) X509_free(certificate);
    }
    SSL_shutdown(tls);
    SSL_free(tls);
    SSL_CTX_free(context);
    close(socket_fd);
    return success;
}

int chat_priority(const nxgallery::TelegramChat &chat) {
    if (chat.type == "channel") return 0;
    if (chat.type == "private") return 1;
    if (chat.type == "group" || chat.type == "supergroup") return 2;
    return 3;
}

void probe_line(const char *phase, bool success, const std::string &detail = {}) {
    std::printf("NXGALLERY_PROBE phase=%s result=%s", phase, success ? "pass" : "fail");
    if (!detail.empty()) std::printf(" detail=%s", detail.c_str());
    std::printf("\n");
}

void probe_horizon_album() {
    SetSysPrimaryAlbumStorage primary{};
    Result result = setsysInitialize();
    if (R_SUCCEEDED(result)) {
        result = setsysGetPrimaryAlbumStorage(&primary);
        std::printf("NXGALLERY_PROBE phase=primary_album_storage result=%s storage=%s rc=0x%08x\n",
                    R_SUCCEEDED(result) ? "pass" : "fail",
                    primary == SetSysPrimaryAlbumStorage_SdCard ? "sd" : "nand",
                    static_cast<unsigned int>(result));
        setsysExit();
    } else {
        std::printf("NXGALLERY_PROBE phase=primary_album_storage result=fail rc=0x%08x\n",
                    static_cast<unsigned int>(result));
    }

    result = capsaInitialize();
    std::printf("NXGALLERY_PROBE phase=capsa_init result=%s rc=0x%08x\n",
                R_SUCCEEDED(result) ? "pass" : "fail",
                static_cast<unsigned int>(result));
    if (R_FAILED(result)) return;
    for (const CapsAlbumStorage storage : {CapsAlbumStorage_Nand, CapsAlbumStorage_Sd}) {
        bool mounted = false;
        u64 count = 0;
        const Result mount_result = capsaIsAlbumMounted(storage, &mounted);
        const Result count_result = capsaGetAlbumFileCount(storage, &count);
        std::printf("NXGALLERY_PROBE phase=album_store result=%s storage=%s mounted=%s count=%llu mount_rc=0x%08x count_rc=0x%08x\n",
                    R_SUCCEEDED(mount_result) && R_SUCCEEDED(count_result) ? "pass" : "fail",
                    storage == CapsAlbumStorage_Sd ? "sd" : "nand",
                    mounted ? "true" : "false", static_cast<unsigned long long>(count),
                    static_cast<unsigned int>(mount_result),
                    static_cast<unsigned int>(count_result));
    }
    capsaExit();
}

int run_probe(const nxgallery::AlbumScanResult &album, nxgallery::TelegramBot *bot,
              const std::string &telegram_status, bool send_media) {
    std::printf("NXGALLERY_PROBE_BEGIN version=1\n");
    probe_horizon_album();
    if (!album) {
        probe_line("sd_scan", false, album.error);
        std::printf("NXGALLERY_PROBE_RESULT result=fail sd=fail network=skip photo=skip video=skip\n");
        return 3;
    }

    const nxgallery::MediaItem *photo = nullptr;
    const nxgallery::MediaItem *video = nullptr;
    std::size_t photo_count = 0;
    std::size_t video_count = 0;
    for (const auto &media : album.items) {
        if (media.kind == nxgallery::MediaKind::Photo) {
            ++photo_count;
            if (photo == nullptr) photo = &media;
        } else {
            ++video_count;
            if (video == nullptr) video = &media;
        }
    }
    const bool sd_ready = photo != nullptr && video != nullptr;
    probe_line("sd_scan", sd_ready,
               "photos=" + std::to_string(photo_count) +
               ",videos=" + std::to_string(video_count) +
               (album.truncated ? ",truncated=true" : ",truncated=false"));
    bool playback_ready = false;
    if (video != nullptr) {
        nxgallery::VideoPlayer player(nullptr);
        player.play(*video);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (player.frames_decoded() < 3 && player.active() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const std::uint64_t before_pause = player.frames_decoded();
        if (before_pause >= 3 && player.active()) {
            player.toggle_pause();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::uint64_t paused_frames = player.frames_decoded();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const std::uint64_t during_pause = player.frames_decoded();
            player.toggle_pause();
            const auto resume_deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(3);
            while (player.frames_decoded() <= during_pause && player.active() &&
                   std::chrono::steady_clock::now() < resume_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            playback_ready = during_pause == paused_frames &&
                player.frames_decoded() > during_pause;
        }
        player.stop();
    }
    probe_line("video_playback", playback_ready,
               "pause_resume=" + std::string(playback_ready ? "pass" : "fail"));
    if (bot == nullptr) {
        probe_line("telegram_init", false,
                   telegram_status.empty() ? "Telegram unavailable" : telegram_status);
        std::printf("NXGALLERY_PROBE_RESULT result=fail sd=%s playback=%s network=fail photo=skip video=skip\n",
                    sd_ready ? "pass" : "fail", playback_ready ? "pass" : "fail");
        return 3;
    }

    std::vector<nxgallery::TelegramChat> chats;
    const nxgallery::BotResult refresh = bot->refresh_chats(chats);
    std::stable_sort(chats.begin(), chats.end(), [](const auto &left, const auto &right) {
        return chat_priority(left) < chat_priority(right);
    });
    probe_line("chat_refresh", !chats.empty(),
               "destinations=" + std::to_string(chats.size()) +
               ",api_result=" + (refresh.success ? "pass" : "fail") +
               ",message=" + refresh.message);

    const bool chats_ready = !chats.empty() && refresh.success;
    if (!send_media) {
        const bool passed = sd_ready && playback_ready && chats_ready;
        std::printf("NXGALLERY_PROBE_RESULT result=%s sd=%s playback=%s chats=%s photo=skip video=skip\n",
                    passed ? "pass" : "fail", sd_ready ? "pass" : "fail",
                    playback_ready ? "pass" : "fail", chats_ready ? "pass" : "fail");
        return passed ? 0 : 3;
    }

    bool photo_sent = false;
    bool video_sent = false;
    const nxgallery::TelegramChat *destination = nullptr;
    if (photo != nullptr) {
        for (std::size_t index = 0; index < chats.size(); ++index) {
            std::printf("NXGALLERY_PROBE phase=photo_attempt destination=%zu type=%s\n",
                        index, chats[index].type.c_str());
            const nxgallery::BotResult result = bot->send_media(*photo, chats[index]);
            probe_line("photo_send", result.success, result.success ? "delivered" : result.message);
            if (result.success) {
                photo_sent = true;
                destination = &chats[index];
                break;
            }
        }
    } else {
        probe_line("photo_send", false, "No photo capture found");
    }

    if (video != nullptr && destination != nullptr) {
        const nxgallery::BotResult result = bot->send_media(*video, *destination);
        video_sent = result.success;
        probe_line("video_send", result.success, result.success ? "delivered" : result.message);
    } else {
        probe_line("video_send", false,
                   video == nullptr ? "No video capture found" : "No working destination found");
    }

    const bool network_ready = photo_sent || video_sent;
    const bool passed = sd_ready && playback_ready && network_ready && photo_sent && video_sent;
    std::printf("NXGALLERY_PROBE_RESULT result=%s sd=%s playback=%s network=%s photo=%s video=%s\n",
                passed ? "pass" : "fail", sd_ready ? "pass" : "fail",
                playback_ready ? "pass" : "fail",
                network_ready ? "pass" : "fail", photo_sent ? "pass" : "fail",
                video_sent ? "pass" : "fail");
    return passed ? 0 : 3;
}
}

int main(int argc, char **argv) {
    nxgallery::install_crash_diagnostics();
    const ProbeOptions probe = parse_probe_options(argc, argv);
    const bool probe_mode = probe.enabled;
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.num_bsd_sessions = 8;
    const Result socket_result = socketInitialize(&socket_config);
    const int nxlink_socket = R_SUCCEEDED(socket_result) ? nxlinkStdio() : -1;
    if (nxlink_socket >= 0) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::printf("NXGALLERY_DIAGNOSTIC event=startup probe=%s\n", probe_mode ? "true" : "false");
    }
    bool curl_ready = false;
    bool release_updates_enabled = false;
    std::string telegram_status;
    if (R_SUCCEEDED(socket_result)) {
        unsigned char seed[64];
        randomGet(seed, sizeof(seed));
        RAND_seed(seed, sizeof(seed));
        OPENSSL_cleanse(seed, sizeof(seed));
        curl_ready = RAND_status() == 1 && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    } else {
        telegram_status = "Network initialization failed";
    }

    std::unique_ptr<nxgallery::TelegramBot> bot;
    if (curl_ready) {
        (void)nxgallery::ensure_https_ca_file();
        const nxgallery::HttpsTrustStatus trust = nxgallery::preflight_https_ca_file();
        if (trust != nxgallery::HttpsTrustStatus::Available) {
            telegram_status = nxgallery::https_trust_diagnostic(trust);
        } else {
            release_updates_enabled = true;
            auto config = nxgallery::load_telegram_config(kTelegramConfigPath);
            if (!probe.config_url.empty()) {
                std::string probe_contents;
                std::string probe_error;
                if (fetch_probe_config(probe, probe_contents, probe_error)) {
                    config = nxgallery::parse_telegram_config(probe_contents);
                    if (config && !probe_mode) {
                        (void)mkdir("sdmc:/switch", 0777);
                        (void)mkdir("sdmc:/switch/nxgallery", 0777);
                        std::FILE *out = std::fopen(kTelegramConfigPath, "wb");
                        const bool persisted = out != nullptr &&
                            std::fwrite(probe_contents.data(), 1, probe_contents.size(),
                                        out) == probe_contents.size();
                        if (out != nullptr) std::fclose(out);
                        std::printf("NXGALLERY_DIAGNOSTIC event=telegram_config_persist path=%s result=%s\n",
                                    kTelegramConfigPath, persisted ? "pass" : "fail");
                    }
                    OPENSSL_cleanse(probe_contents.data(), probe_contents.size());
                } else {
                    config.error = std::move(probe_error);
                }
            }
            if (config) {
                std::printf("NXGALLERY_DIAGNOSTIC event=telegram_config source=%s configured_chats=%zu discover=%s\n",
                            probe.config_url.empty() ? "sd" : "injected",
                            config.config->chats.size(),
                            config.config->discover_chats ? "true" : "false");
                bot = std::make_unique<nxgallery::TelegramBot>(std::move(*config.config));
            } else {
                telegram_status = config.error;
                std::printf("NXGALLERY_DIAGNOSTIC event=telegram_config source=%s result=fail line=%zu\n",
                            probe.config_url.empty() ? "sd" : "injected", config.line);
            }
        }
    }

    nxgallery::AlbumScanResult album;
#ifdef NXGALLERY_AUTOMATION_BUILD
    album = nxgallery::scan_album(kRawAlbumPath);
#else
    album = nxgallery::scan_horizon_album();
#endif
    if (!probe_mode && (!album || album.items.empty())) {
        nxgallery::AlbumScanResult raw_album = nxgallery::scan_album(kRawAlbumPath);
        if (raw_album && !raw_album.items.empty()) album = std::move(raw_album);
    }
    if (probe_mode) {
        const int result = run_probe(album, bot.get(), telegram_status,
                                     probe.send_media);
        std::printf("NXGALLERY_DIAGNOSTIC event=probe_exit logical_code=%d process_code=0\n", result);
        std::fflush(stdout);
        std::fflush(stderr);
        if (curl_ready) curl_global_cleanup();
        nxgallery::shutdown_horizon_album();
        if (nxlink_socket >= 0) close(nxlink_socket);
        if (R_SUCCEEDED(socket_result)) socketExit();
        return 0;
    }
    pu::ui::render::RendererInitOptions options(
        SDL_INIT_VIDEO, pu::ui::render::RendererHardwareFlags, 1280, 720);
    options.SetPlServiceType(PlServiceType_User);
    options.AddDefaultSharedFont(PlSharedFontType_Standard);
    options.AddDefaultSharedFont(PlSharedFontType_KO);
    options.AddDefaultSharedFont(PlSharedFontType_NintendoExt);
    for (const std::uint32_t size : {16U, 17U, 18U, 19U, 20U, 22U, 23U,
                                     24U, 25U, 29U, 32U, 34U, 48U}) {
        options.AddExtraDefaultFontSize(size);
    }
    options.SetInputPlayerCount(1);
    options.AddInputNpadIdType(HidNpadIdType_Handheld);
    options.AddInputNpadIdType(HidNpadIdType_No1);
    options.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
    auto renderer = pu::ui::render::Renderer::New(options);
    auto application = std::make_shared<nxgallery::GalleryApplication>(
        renderer, std::move(album), std::move(bot), std::move(telegram_status),
        release_updates_enabled);
    const Result load_result = application->Load();
    if (R_SUCCEEDED(load_result)) application->Show();
    application.reset();
    renderer.reset();
    nxgallery::shutdown_horizon_album();
    if (curl_ready) curl_global_cleanup();
    if (nxlink_socket >= 0) close(nxlink_socket);
    if (R_SUCCEEDED(socket_result)) socketExit();
    return R_SUCCEEDED(load_result) ? 0 : 2;
}
