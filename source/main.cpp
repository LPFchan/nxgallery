#include <nxgallery/album_index.hpp>
#include <nxgallery/gallery_app.hpp>
#include <nxgallery/https_trust.hpp>
#include <nxgallery/telegram_config.hpp>

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <pu/Plutonium>
#include <switch.h>

#include <cstdint>
#include <memory>
#include <string>

// Plutonium's SDL renderer needs substantially more than libnx's default heap.
u64 __nx_heap_size = 256ULL * 1024ULL * 1024ULL;

namespace {
constexpr char kAlbumPath[] = "sdmc:/Nintendo/Album";
constexpr char kTelegramConfigPath[] = "sdmc:/switch/nxgallery/telegram-bot.conf";
}

int main(int, char **) {
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.num_bsd_sessions = 8;
    const Result socket_result = socketInitialize(&socket_config);
    bool curl_ready = false;
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
        const nxgallery::HttpsTrustStatus trust = nxgallery::preflight_https_ca_file();
        if (trust != nxgallery::HttpsTrustStatus::Available) {
            telegram_status = nxgallery::https_trust_diagnostic(trust);
        } else {
            auto config = nxgallery::load_telegram_config(kTelegramConfigPath);
            if (config) bot = std::make_unique<nxgallery::TelegramBot>(std::move(*config.config));
            else telegram_status = config.error;
        }
    }

    nxgallery::AlbumScanResult album = nxgallery::scan_album(kAlbumPath);
    pu::ui::render::RendererInitOptions options(
        SDL_INIT_VIDEO, pu::ui::render::RendererHardwareFlags, 1280, 720);
    options.SetPlServiceType(PlServiceType_User);
    options.AddDefaultSharedFont(PlSharedFontType_Standard);
    options.AddDefaultSharedFont(PlSharedFontType_KO);
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
        renderer, std::move(album), std::move(bot), std::move(telegram_status));
    const Result load_result = application->Load();
    if (R_SUCCEEDED(load_result)) application->Show();
    application.reset();
    renderer.reset();
    if (curl_ready) curl_global_cleanup();
    if (R_SUCCEEDED(socket_result)) socketExit();
    return R_SUCCEEDED(load_result) ? 0 : 2;
}
