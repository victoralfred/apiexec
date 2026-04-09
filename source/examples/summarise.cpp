// Example: chunked document summarisation with token budget enforcement.
//
// Demonstrates the AIAdapter prompt chunking and CostAwarePolicy budget
// cap working together. Uses a mock server since this is a build-time example.
//
// Usage: ./summarise [budget_tokens]
//   Default: 500 tokens

#include "core/engine.hpp"
#include "adapters/openai.hpp"
#include "policy/cost_aware_policy.hpp"
#include "transport/curl_transport.hpp"
#include "adapters/ai_adapter.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

// Inline mock server for the example
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace apiexec;

constexpr int TOKENS_PER_RESPONSE = 30;

static std::atomic<bool> g_running{true};

static auto serve_mock(int fd) -> void {
    while (g_running.load()) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int client = accept(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (client < 0) continue;

        char buf[8192];
        recv(client, buf, sizeof(buf) - 1, 0);

        // Parse prompt from request body
        std::string raw(buf);
        auto body_start = raw.find("\r\n\r\n");
        std::string prompt;
        if (body_start != std::string::npos) {
            auto j = nlohmann::json::parse(raw.substr(body_start + 4), nullptr, false);
            if (!j.is_discarded() && j.contains("messages")) {
                prompt = j["messages"][0].value("content", "");
            }
        }

        // Generate mock response
        nlohmann::json resp;
        resp["choices"] = nlohmann::json::array({
            {{"index", 0}, {"message", {{"role", "assistant"},
                {"content", "Summary of: " + prompt.substr(0, 50) + "..."}}}}
        });
        resp["usage"] = {
            {"prompt_tokens", 10},
            {"completion_tokens", 20},
            {"total_tokens", TOKENS_PER_RESPONSE}
        };

        auto body = resp.dump();
        std::ostringstream http;
        http << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
             << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n"
             << body;
        auto s = http.str();
        send(client, s.data(), s.size(), 0);
        close(client);
    }
}

auto main(int argc, char* argv[]) -> int {
    double budget_tokens = 500;
    if (argc > 1) budget_tokens = std::stod(argv[1]);

    // Start mock server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(server_fd, 16);
    socklen_t alen = sizeof(addr);
    getsockname(server_fd, reinterpret_cast<sockaddr*>(&addr), &alen);
    int port = ntohs(addr.sin_port);

    std::thread server_thread(serve_mock, server_fd);

    // Simulate a large document as multiple prompts (chunks)
    std::vector<std::string> chunks;
    for (int i = 0; i < 50; ++i) {
        chunks.push_back("Chunk " + std::to_string(i) + ": Lorem ipsum dolor sit amet, "
                         "consectetur adipiscing elit. Sed do eiusmod tempor incididunt "
                         "ut labore et dolore magna aliqua.");
    }

    // Build OpenAI adapter config
    nlohmann::json config;
    config["base_url"] = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
    config["api_key"] = "example-key";
    config["model"] = "gpt-4";
    config["prompts"] = chunks;
    config["max_tokens"] = 100;

    // Cost-aware policy
    DefaultPolicy::Config base_cfg;
    base_cfg.prefetch_depth_val = 0;

    CostAwarePolicy::CostConfig cost_cfg;
    cost_cfg.budget_tokens = budget_tokens;
    cost_cfg.price_per_token = 0.00003;

    auto* policy = new CostAwarePolicy(base_cfg, cost_cfg);

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<OpenAIAdapter>(OpenAIAdapter::from_json(config.dump())),
        std::unique_ptr<ExecutionPolicy>(policy),
        std::make_unique<CurlTransport>(),
        Cursor{}
    );

    std::cout << "Summarising " << chunks.size() << " chunks with budget="
              << budget_tokens << " tokens\n\n";

    int completed = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error == StreamErrorCode::BUDGET_EXHAUSTED) {
            std::cout << "\n[Budget exhausted after " << completed << " chunks]\n";
            break;
        }
        if (result.error != StreamErrorCode::OK) {
            std::cout << "\n[Error: " << static_cast<int>(result.error) << "]\n";
            break;
        }

        for (const auto& batch : result.records) {
            for (const auto& choice : batch.records) {
                std::string content = choice.value("message", nlohmann::json{})
                                           .value("content", "");
                std::cout << "Chunk " << completed << ": " << content << "\n";
            }
        }
        ++completed;
    }

    std::cout << "\n--- Cost Report ---\n"
              << "Chunks processed: " << completed << " / " << chunks.size() << "\n"
              << "Tokens consumed:  " << policy->cumulative_tokens() << "\n"
              << "Estimated cost:   $" << policy->cumulative_dollars() << "\n";

    auto remaining = policy->remaining_budget();
    if (remaining.has_value()) {
        std::cout << "Remaining budget: " << remaining.value() << " tokens\n";
    }

    // Cleanup
    g_running.store(false);
    int wake = socket(AF_INET, SOCK_STREAM, 0);
    connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(wake);
    server_thread.join();
    close(server_fd);

    return 0;
}
