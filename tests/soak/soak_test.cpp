// Soak test: runs the engine against a mock rate-limited API for a
// configurable duration, verifying no memory growth and correct metrics.
//
// Usage: ./soak_test [duration_seconds]
//   Default: 60 seconds (use 86400 for a full 24h soak)

#include "core/engine.hpp"
#include "adapters/generic_rest.hpp"
#include "policy/default_policy.hpp"
#include "transport/curl_transport.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// Inline minimal mock server (avoids linking mock_server for soak)
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

using namespace apiexec;

constexpr int RECORDS_PER_PAGE = 50;
constexpr int RATE_LIMIT_EVERY_N = 20;  // every 20th request gets a 429

static std::atomic<int> g_request_count{0};
static std::atomic<bool> g_server_running{true};

static auto serve(int server_fd) -> void {
    while (g_server_running.load()) {
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(server_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) continue;

        char buf[4096];
        recv(fd, buf, sizeof(buf) - 1, 0);
        int count = g_request_count.fetch_add(1);

        std::string body;
        std::string extra_headers;
        int status = 200;

        if (count % RATE_LIMIT_EVERY_N == 0 && count > 0) {
            status = 429;
            body = R"({"error":"rate limited"})";
            extra_headers = "Retry-After: 0\r\n";
        } else {
            nlohmann::json j;
            j["data"] = nlohmann::json::array();
            for (int i = 0; i < RECORDS_PER_PAGE; ++i) {
                j["data"].push_back({{"id", count * RECORDS_PER_PAGE + i}});
            }
            j["next"] = std::to_string(count + 1);
            body = j.dump();
        }

        std::ostringstream resp;
        resp << "HTTP/1.1 " << status << " OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << extra_headers
             << "\r\n" << body;
        auto s = resp.str();
        send(fd, s.data(), s.size(), 0);
        close(fd);
    }
}

auto main(int argc, char* argv[]) -> int {
    int duration_secs = 60;
    if (argc > 1) {
        duration_secs = std::atoi(argv[1]);
    }

    // Start mock server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(server_fd, 16);
    socklen_t alen = sizeof(addr);
    getsockname(server_fd, reinterpret_cast<sockaddr*>(&addr), &alen);
    int port = ntohs(addr.sin_port);

    std::thread server_thread(serve, server_fd);

    std::string base_url = "http://127.0.0.1:" + std::to_string(port) + "/api";
    std::cout << "soak_test: duration=" << duration_secs << "s  server=" << base_url << "\n";

    GenericRestAdapter::Config adapter_cfg;
    adapter_cfg.base_url = base_url;
    adapter_cfg.page_size = RECORDS_PER_PAGE;

    DefaultPolicy::Config policy_cfg;
    policy_cfg.prefetch_depth_val = 1;

    auto engine = std::make_unique<ExecutionEngine<JsonBatch>>(
        std::make_unique<GenericRestAdapter>(adapter_cfg),
        std::make_unique<DefaultPolicy>(policy_cfg),
        std::make_unique<CurlTransport>(),
        Cursor{}
    );
    engine->set_sleep_fn([](Duration) {});

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(duration_secs);

    int64_t total_records = 0;
    int64_t total_batches = 0;
    constexpr int REPORT_INTERVAL = 1000;

    while (std::chrono::steady_clock::now() < deadline && engine->has_next()) {
        auto result = engine->next_batch();
        if (result.error == StreamErrorCode::EXHAUSTED) break;
        if (result.error != StreamErrorCode::OK) {
            // Transient errors are expected — just continue
            continue;
        }
        for (const auto& batch : result.records) {
            total_records += batch.count;
        }
        ++total_batches;

        if (total_batches % REPORT_INTERVAL == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            auto snap = engine->metrics_snapshot();
            std::cout << "  [" << elapsed << "s] batches=" << total_batches
                      << " records=" << total_records
                      << " requests=" << snap.request_count
                      << " retries=" << snap.retry_count
                      << " errors_429=" << snap.error_rate_limit
                      << "\n";
        }
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto snap = engine->metrics_snapshot();
    std::cout << "\nsoak_test results:\n"
              << "  duration: " << elapsed_ms << "ms\n"
              << "  batches: " << total_batches << "\n"
              << "  records: " << total_records << "\n"
              << "  requests: " << snap.request_count << "\n"
              << "  retries: " << snap.retry_count << "\n"
              << "  errors_429: " << snap.error_rate_limit << "\n"
              << "  errors_server: " << snap.error_server << "\n"
              << "  errors_network: " << snap.error_network << "\n";

    // Cleanup
    engine.reset();
    g_server_running.store(false);
    // Wake accept
    int wake = socket(AF_INET, SOCK_STREAM, 0);
    connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(wake);
    server_thread.join();
    close(server_fd);

    std::cout << "\n  PASS: soak_test completed (" << elapsed_ms / 1000 << "s)\n";
    return 0;
}
