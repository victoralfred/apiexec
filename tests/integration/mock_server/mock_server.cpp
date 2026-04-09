#include "mock_server.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace apiexec::test {

MockServer::MockServer() = default;

MockServer::~MockServer() {
    stop();
}

void MockServer::set_handler(Handler h) {
    handler_ = std::move(h);
}

void MockServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) throw std::runtime_error("socket() failed");

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // OS assigns port

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd_);
        throw std::runtime_error("bind() failed");
    }

    if (listen(server_fd_, 16) < 0) {
        close(server_fd_);
        throw std::runtime_error("listen() failed");
    }

    // Get assigned port
    socklen_t len = sizeof(addr);
    getsockname(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);

    running_.store(true);
    thread_ = std::thread(&MockServer::serve_loop, this);
}

void MockServer::stop() {
    if (running_.load()) {
        running_.store(false);
        // Connect to self to unblock accept()
        int wake_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (wake_fd >= 0) {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port_);
            connect(wake_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            close(wake_fd);
        }
        if (thread_.joinable()) thread_.join();
        close(server_fd_);
        server_fd_ = -1;
    }
}

std::string MockServer::url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

uint16_t MockServer::port() const {
    return port_;
}

void MockServer::serve_loop() {
    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0 || !running_.load()) {
            if (client_fd >= 0) close(client_fd);
            break;
        }
        handle_connection(client_fd);
        close(client_fd);
    }
}

void MockServer::handle_connection(int client_fd) {
    // Read the full request (up to 64KB)
    char buf[65536];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    std::string raw(buf, n);

    // Parse method and path from first line
    std::string method, path;
    std::istringstream first_line(raw);
    first_line >> method >> path;

    // Parse body (after \r\n\r\n)
    std::string body;
    auto body_start = raw.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        body = raw.substr(body_start + 4);
    }

    // Dispatch to handler
    Response resp;
    if (handler_) {
        resp = handler_(method, path, body);
    } else {
        resp.status_code = 404;
        resp.body = R"({"error":"no handler"})";
    }

    std::string http_resp = build_http_response(resp);
    send(client_fd, http_resp.data(), http_resp.size(), 0);
}

std::string MockServer::build_http_response(const Response& resp) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status_code << " ";

    // Status text
    switch (resp.status_code) {
        case 200: ss << "OK"; break;
        case 429: ss << "Too Many Requests"; break;
        case 500: ss << "Internal Server Error"; break;
        default:  ss << "Status"; break;
    }
    ss << "\r\n";

    ss << "Content-Type: " << resp.content_type << "\r\n";
    ss << "Content-Length: " << resp.body.size() << "\r\n";
    ss << "Connection: close\r\n";

    if (!resp.extra_headers.empty()) {
        ss << resp.extra_headers;
        // Ensure trailing \r\n if not present
        if (resp.extra_headers.back() != '\n') ss << "\r\n";
    }

    ss << "\r\n";
    ss << resp.body;
    return ss.str();
}

} // namespace apiexec::test
