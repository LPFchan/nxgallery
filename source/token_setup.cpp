#include <nxgallery/token_setup.hpp>

#include <nxgallery/telegram_config.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace nxgallery {
namespace {

constexpr std::size_t kMaxHeaderBytes = 8 * 1024;
constexpr std::size_t kMaxBodyBytes = 8 * 1024;
constexpr std::size_t kMaxRequestBytes = kMaxHeaderBytes + kMaxBodyBytes;

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

bool header_name_is(std::string_view line, std::string_view name) {
    if (line.size() < name.size() + 1 || line[name.size()] != ':') return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char left = line[index];
        const char lowered = left >= 'A' && left <= 'Z'
            ? static_cast<char>(left - 'A' + 'a') : left;
        if (lowered != name[index]) return false;
    }
    return true;
}

std::string http_response(std::string_view status, std::string_view content_type,
                          std::string_view body) {
    std::string response = "HTTP/1.1 ";
    response.append(status);
    response.append("\r\nContent-Type: ");
    response.append(content_type);
    response.append("\r\nContent-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    response.append(body);
    return response;
}

std::string page_shell(std::string_view heading, std::string_view content) {
    std::string page =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>NX Gallery setup</title><style>"
        "body{font-family:-apple-system,system-ui,sans-serif;margin:0;"
        "background:#ebebeb;color:#2d2d2d;display:flex;justify-content:center}"
        "main{max-width:26rem;width:100%;padding:2rem 1.25rem;box-sizing:border-box}"
        "h1{font-size:1.35rem}p{line-height:1.5}"
        "input{width:100%;box-sizing:border-box;font-size:1rem;padding:.8rem;"
        "border:1px solid #c9c9cd;border-radius:8px;background:#fff;color:inherit}"
        "button{width:100%;margin-top:1rem;font-size:1.05rem;padding:.85rem;"
        "border:0;border-radius:8px;background:#3250f0;color:#fff}"
        ".notice{color:#c83741}"
        "@media(prefers-color-scheme:dark){body{background:#2d2d2d;color:#ebebeb}"
        "input{background:#1c1c1e;border-color:#4a4a4e}}"
        "</style></head><body><main><h1>";
    page.append(heading);
    page.append("</h1>");
    page.append(content);
    page.append("</main></body></html>");
    return page;
}

std::string token_form_page(std::string_view notice) {
    std::string content =
        "<p>Paste the bot token from <b>@BotFather</b> in Telegram. "
        "It travels only over your local network and is stored only on the "
        "Switch's SD card.</p>";
    if (!notice.empty()) {
        content.append("<p class=\"notice\">");
        content.append(notice);
        content.append("</p>");
    }
    content.append(
        "<form method=\"post\"><input name=\"token\" "
        "placeholder=\"123456789:AbCdEf...\" autocomplete=\"off\" "
        "autocapitalize=\"none\" autocorrect=\"off\" spellcheck=\"false\" "
        "required><button>Send to Switch</button></form>");
    return page_shell("Connect NX Gallery to Telegram", content);
}

std::string token_received_page() {
    return page_shell("Token sent",
                      "<p>Done &mdash; return to your Switch.</p>"
                      "<p>If the token turns out to be wrong, the Switch will "
                      "show an error and you can run setup again.</p>");
}

void cleanse(std::string &value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

bool send_all(int socket_fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t sent = send(socket_fd, data.data(), data.size(), 0);
        if (sent <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

}  // namespace

bool parse_http_request(std::string_view buffer, HttpRequest &request,
                        bool &malformed) noexcept {
    malformed = false;
    const std::size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        malformed = buffer.size() > kMaxHeaderBytes;
        return false;
    }
    const std::string_view head = buffer.substr(0, header_end);
    const std::size_t line_end = head.find("\r\n");
    const std::string_view request_line =
        line_end == std::string_view::npos ? head : head.substr(0, line_end);
    const std::size_t method_end = request_line.find(' ');
    const std::size_t target_end =
        method_end == std::string_view::npos
            ? std::string_view::npos : request_line.find(' ', method_end + 1);
    if (method_end == std::string_view::npos || method_end == 0 ||
        target_end == std::string_view::npos || target_end == method_end + 1 ||
        request_line.substr(target_end + 1).rfind("HTTP/", 0) != 0) {
        malformed = true;
        return false;
    }

    std::size_t content_length = 0;
    std::string_view headers =
        line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
    while (!headers.empty()) {
        const std::size_t next = headers.find("\r\n");
        const std::string_view line =
            next == std::string_view::npos ? headers : headers.substr(0, next);
        headers = next == std::string_view::npos ? std::string_view{} : headers.substr(next + 2);
        if (!header_name_is(line, "content-length")) continue;
        const std::string_view value = trim(line.substr(sizeof("content-length")));
        if (value.empty()) {
            malformed = true;
            return false;
        }
        content_length = 0;
        for (const char c : value) {
            if (c < '0' || c > '9' || content_length > kMaxBodyBytes) {
                malformed = true;
                return false;
            }
            content_length = content_length * 10 + static_cast<std::size_t>(c - '0');
        }
        if (content_length > kMaxBodyBytes) {
            malformed = true;
            return false;
        }
    }
    if (buffer.size() < header_end + 4 + content_length) return false;
    request.method.assign(request_line.substr(0, method_end));
    request.target.assign(request_line.substr(method_end + 1, target_end - method_end - 1));
    request.body.assign(buffer.substr(header_end + 4, content_length));
    return true;
}

std::string url_decode_form_value(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (c == '+') {
            decoded.push_back(' ');
        } else if (c == '%' && index + 2 < value.size()) {
            auto hex = [](char digit) -> int {
                if (digit >= '0' && digit <= '9') return digit - '0';
                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                return -1;
            };
            const int high = hex(value[index + 1]);
            const int low = hex(value[index + 2]);
            if (high < 0 || low < 0) {
                decoded.push_back(c);
                continue;
            }
            decoded.push_back(static_cast<char>(high * 16 + low));
            index += 2;
        } else {
            decoded.push_back(c);
        }
    }
    return decoded;
}

std::string form_field(std::string_view body, std::string_view name) {
    while (!body.empty()) {
        const std::size_t ampersand = body.find('&');
        const std::string_view pair =
            ampersand == std::string_view::npos ? body : body.substr(0, ampersand);
        body = ampersand == std::string_view::npos
            ? std::string_view{} : body.substr(ampersand + 1);
        const std::size_t equals = pair.find('=');
        const std::string_view key =
            equals == std::string_view::npos ? pair : pair.substr(0, equals);
        if (url_decode_form_value(key) != name) continue;
        return equals == std::string_view::npos
            ? std::string{} : url_decode_form_value(pair.substr(equals + 1));
    }
    return {};
}

TokenSetupServer::~TokenSetupServer() { stop(); }

bool TokenSetupServer::start(const std::string &display_host, std::uint16_t port,
                             const std::string &path_secret, std::string &error) {
    if (worker_.joinable()) {
        error = "Setup is already running";
        return false;
    }
    path_ = "/s/" + path_secret;
    url_ = "http://" + display_host + ":" + std::to_string(port) + path_;
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        error = "Could not create the setup socket";
        return false;
    }
    const int enable = 1;
    (void)setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listen_fd_, 4) != 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        error = "Could not open the setup port";
        return false;
    }
    stop_requested_ = false;
    state_ = State::Waiting;
    worker_ = std::thread([this] { serve(); });
    return true;
}

void TokenSetupServer::stop() {
    stop_requested_ = true;
    if (worker_.joinable()) worker_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cleanse(token_);
}

std::string TokenSetupServer::take_token() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string token = std::move(token_);
    token_.clear();
    return token;
}

std::string TokenSetupServer::failure_reason() {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_;
}

std::string TokenSetupServer::handle_request(const HttpRequest &request,
                                             bool &received) {
    const std::string_view target =
        std::string_view(request.target).substr(0, request.target.find('?'));
    if (target != path_) {
        return http_response("404 Not Found", "text/plain; charset=utf-8",
                             "Not found\n");
    }
    if (request.method == "GET") {
        return http_response("200 OK", "text/html; charset=utf-8",
                             token_form_page({}));
    }
    if (request.method != "POST") {
        return http_response("405 Method Not Allowed", "text/plain; charset=utf-8",
                             "Method not allowed\n");
    }
    std::string token(trim(form_field(request.body, "token")));
    // The config parser is the single authority on token syntax; a token that
    // smuggles newlines fails right here instead of corrupting the saved file.
    auto probe = parse_telegram_config("bot_token = " + token + "\n");
    if (!probe) {
        cleanse(token);
        return http_response(
            "200 OK", "text/html; charset=utf-8",
            token_form_page("That does not look like a bot token. It should "
                            "look like <b>123456789:AbCdEf...</b> &mdash; "
                            "copy it exactly from @BotFather."));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_ = std::move(token);
    }
    cleanse(probe.config->bot_token);
    received = true;
    return http_response("200 OK", "text/html; charset=utf-8",
                         token_received_page());
}

void TokenSetupServer::serve() {
    while (!stop_requested_.load()) {
        pollfd entry{listen_fd_, POLLIN, 0};
        const int ready = poll(&entry, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = "The setup listener stopped unexpectedly";
            state_ = State::Failed;
            return;
        }
        if (ready == 0) continue;
        const int client = accept(listen_fd_, nullptr, nullptr);
        if (client < 0) continue;
        const timeval timeout{5, 0};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string buffer;
        HttpRequest request;
        bool malformed = false;
        bool complete = false;
        std::array<char, 2048> chunk{};
        while (buffer.size() < kMaxRequestBytes) {
            const ssize_t bytes = recv(client, chunk.data(), chunk.size(), 0);
            if (bytes <= 0) break;
            buffer.append(chunk.data(), static_cast<std::size_t>(bytes));
            if (parse_http_request(buffer, request, malformed)) {
                complete = true;
                break;
            }
            if (malformed) break;
        }
        bool received = false;
        std::string response = complete
            ? handle_request(request, received)
            : http_response("400 Bad Request", "text/plain; charset=utf-8",
                            "Bad request\n");
        (void)send_all(client, response);
        (void)shutdown(client, SHUT_RDWR);
        close(client);
        std::fill(chunk.begin(), chunk.end(), '\0');
        cleanse(buffer);
        cleanse(request.body);
        if (received) {
            state_ = State::Received;
            return;
        }
    }
}

}  // namespace nxgallery
