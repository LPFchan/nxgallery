#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace nxgallery {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string body;
};

// Parses one complete HTTP/1.x request out of a receive buffer. Returns false
// while the buffer is still incomplete; sets malformed when the buffered data
// can never become an acceptable request.
bool parse_http_request(std::string_view buffer, HttpRequest &request,
                        bool &malformed) noexcept;
std::string url_decode_form_value(std::string_view value);
std::string form_field(std::string_view body, std::string_view name);

// One-shot LAN listener that serves a single paste-the-token page. The QR code
// shown on the console encodes url(); the phone browser fetches the form and
// posts the bot token back. The token is handed to the caller and never leaves
// the local network or this process.
class TokenSetupServer {
public:
    enum class State { Idle, Waiting, Received, Failed };

    ~TokenSetupServer();

    bool start(const std::string &display_host, std::uint16_t port,
               const std::string &path_secret, std::string &error);
    void stop();

    State state() const noexcept { return state_.load(); }
    const std::string &url() const noexcept { return url_; }
    std::string take_token();
    std::string failure_reason();

private:
    void serve();
    std::string handle_request(const HttpRequest &request, bool &received);

    std::thread worker_;
    std::mutex mutex_;
    std::atomic<State> state_{State::Idle};
    std::string url_;
    std::string path_;
    std::string token_;
    std::string failure_;
    int listen_fd_{-1};
    std::atomic<bool> stop_requested_{};
};

}  // namespace nxgallery
