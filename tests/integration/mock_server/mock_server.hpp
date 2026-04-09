#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace apiexec::test {

// Minimal single-threaded HTTP mock server for integration tests.
// Listens on localhost:0 (OS-assigned port) and serves pre-programmed responses.
//
// Usage:
//   MockServer server;
//   server.set_handler([](const std::string& path) -> MockServer::Response { ... });
//   server.start();
//   // use server.url() to connect
//   server.stop();

class MockServer {
public:
    struct Response {
        int status_code = 200;
        std::string body;
        std::string content_type = "application/json";
        std::string extra_headers;  // raw header lines, \r\n separated
    };

    using Handler = std::function<Response(const std::string& method,
                                           const std::string& path,
                                           const std::string& body)>;

    MockServer();
    ~MockServer();

    // Set the request handler.
    void set_handler(Handler h);

    // Start listening. Returns once the server is accepting connections.
    void start();

    // Stop the server and join the thread.
    void stop();

    // Base URL (e.g., "http://127.0.0.1:12345").
    std::string url() const;

    // Port the server is listening on.
    uint16_t port() const;

private:
    void serve_loop();
    void handle_connection(int client_fd);
    std::string build_http_response(const Response& resp);

    Handler handler_;
    int server_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace apiexec::test
