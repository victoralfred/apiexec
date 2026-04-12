// Example: talk to a local Ollama instance using apiexec.
//
// Demonstrates:
//   - OpenAI-compatible adapter against Ollama
//   - CostAwarePolicy with token budget enforcement
//   - Real-time metrics callback with Prometheus format
//   - Structured logging on every error path
//   - Multi-prompt streaming with prefetch
//
// Usage: ./ollama_demo [ollama_host] [model] [budget_tokens]
//   Defaults: 192.168.1.104:11434  qwen3:8b  500
//
// Requires: Ollama running with a model loaded.

#include "core/engine.hpp"
#include "core/logging.hpp"
#include "adapters/openai.hpp"
#include "policy/cost_aware_policy.hpp"
#include "transport/curl_transport.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace apiexec;

constexpr double DEFAULT_BUDGET_TOKENS = 500.0;
constexpr double PRICE_PER_TOKEN = 0.0;  // Ollama is free, but we track tokens
constexpr int MAX_TOKENS_PER_RESPONSE = 100;

auto main(int argc, char* argv[]) -> int {
    // --- Parse arguments ---
    std::string host = "192.168.1.104:11434";
    std::string model = "qwen3:8b";
    double budget = DEFAULT_BUDGET_TOKENS;

    if (argc > 1) host = argv[1];
    if (argc > 2) model = argv[2];
    if (argc > 3) budget = std::stod(argv[3]);

    std::string base_url = "http://" + host + "/v1/chat/completions";

    // --- Prompts to send ---
    std::vector<std::string> prompts = {
        "What is the capital of France? Answer in one sentence.",
        "Explain what an API is in two sentences.",
        "What is the speed of light? One sentence.",
        "Name three programming languages created after 2010.",
        "What is the difference between TCP and UDP? Brief answer.",
        "Who wrote Romeo and Juliet?",
        "What is a hash table? One sentence.",
        "Name the four largest planets in our solar system.",
        "What year did the first iPhone launch?",
        "What does CPU stand for?",
    };

    std::cout << "=== apiexec Ollama Demo ===\n"
              << "Host:    " << host << "\n"
              << "Model:   " << model << "\n"
              << "Prompts: " << prompts.size() << "\n"
              << "Budget:  " << budget << " tokens\n"
              << "Max per response: " << MAX_TOKENS_PER_RESPONSE << " tokens\n\n";

    // --- Build adapter config ---
    nlohmann::json config;
    config["base_url"] = base_url;
    config["api_key"] = "ollama";  // Ollama doesn't check this, but the field is required
    config["model"] = model;
    config["prompts"] = prompts;
    config["max_tokens"] = MAX_TOKENS_PER_RESPONSE;
    config["temperature"] = 0.7;

    // --- Build cost-aware policy ---
    DefaultPolicy::Config policy_cfg;
    policy_cfg.max_retries = 3;
    policy_cfg.base_backoff_ms = 500;
    policy_cfg.prefetch_depth_val = 0;  // sequential for clear output ordering

    CostAwarePolicy::CostConfig cost_cfg;
    cost_cfg.budget_tokens = budget;
    cost_cfg.price_per_token = PRICE_PER_TOKEN;

    auto* policy = new CostAwarePolicy(policy_cfg, cost_cfg);

    // --- Create engine ---
    auto engine = std::make_unique<ExecutionEngine<JsonBatch>>(
        std::make_unique<OpenAIAdapter>(OpenAIAdapter::from_json(config.dump())),
        std::unique_ptr<ExecutionPolicy>(policy),
        std::make_unique<CurlTransport>(),
        Cursor{}
    );

    // --- Set up metrics callback ---
    engine->set_metrics_callback([](const MetricsSnapshot& s) {
        std::cout << "  [metrics] requests=" << s.request_count
                  << " retries=" << s.retry_count
                  << " success=" << s.success_count
                  << " records=" << s.records_total
                  << " window=" << s.window_size_ms << "ms\n";
    });

    // --- Set up structured logging ---
    set_log_level(LogLevel::WARN);
    engine->set_log_callback([](const LogEntry& entry) {
        const char* level_str = "INFO";
        switch (entry.level) {
            case LogLevel::WARN:  level_str = "WARN"; break;
            case LogLevel::ERROR: level_str = "ERROR"; break;
            default: break;
        }
        std::cerr << "  [" << level_str << "] " << entry.message;
        if (entry.http_status > 0) {
            std::cerr << " (HTTP " << entry.http_status << ")";
        }
        if (entry.retry_count > 0) {
            std::cerr << " (retry " << entry.retry_count << ")";
        }
        std::cerr << "\n";
    });

    // --- Stream prompts ---
    auto start = std::chrono::steady_clock::now();
    int completed = 0;

    std::cout << "--- Responses ---\n\n";

    while (engine->has_next()) {
        auto result = engine->next_batch();

        if (result.error == StreamErrorCode::BUDGET_EXHAUSTED) {
            std::cout << "\n[Budget exhausted after " << completed << " prompts]\n";
            break;
        }
        if (result.error == StreamErrorCode::EXHAUSTED) {
            break;
        }
        if (result.error != StreamErrorCode::OK) {
            std::cout << "\n[Error: " << static_cast<int>(result.error) << "]\n";
            break;
        }

        // Extract the assistant's response from the choices.
        // Some models (e.g., qwen3 in thinking mode) put output in "reasoning"
        // instead of "content". Check both.
        for (const auto& batch : result.records) {
            for (const auto& choice : batch.records) {
                std::string content;
                if (choice.contains("message")) {
                    auto& msg = choice["message"];
                    if (msg.contains("content") && !msg["content"].get<std::string>().empty()) {
                        content = msg["content"].get<std::string>();
                    } else if (msg.contains("reasoning")) {
                        content = msg["reasoning"].get<std::string>();
                    }
                }

                std::cout << "Q" << (completed + 1) << ": " << prompts[completed] << "\n"
                          << "A" << (completed + 1) << ": " << content << "\n\n";
            }
        }
        ++completed;

        // Show running cost
        auto remaining = policy->remaining_budget();
        std::cout << "  [cost] tokens_used=" << policy->cumulative_tokens()
                  << " remaining=";
        if (remaining.has_value()) {
            std::cout << remaining.value();
        } else {
            std::cout << "unlimited";
        }
        std::cout << "\n\n";
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // --- Final report ---
    std::cout << "=== Summary ===\n"
              << "Prompts completed: " << completed << " / " << prompts.size() << "\n"
              << "Wall time:         " << elapsed.count() << "ms\n"
              << "Tokens consumed:   " << policy->cumulative_tokens() << "\n";

    if (budget > 0) {
        auto remaining = policy->remaining_budget();
        std::cout << "Budget remaining:  "
                  << (remaining.has_value() ? std::to_string(remaining.value()) : "n/a")
                  << " tokens\n";
        std::cout << "Budget exhausted:  "
                  << (policy->budget_exceeded() ? "yes" : "no") << "\n";
    }

    // --- Prometheus metrics dump ---
    std::cout << "\n=== Prometheus Metrics ===\n"
              << engine->metrics().to_prometheus("ollama_demo");

    return 0;
}
