#include "c_api.h"
#include "mock_server/mock_server.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

using namespace apiexec::test;

constexpr int BUF_SIZE = 1024 * 1024;  // 1 MB
constexpr int GENERIC_REST_PAGES = 100;
constexpr int GENERIC_REST_RECORDS_PER_PAGE = 100;
constexpr int GENERIC_REST_TOTAL = GENERIC_REST_PAGES * GENERIC_REST_RECORDS_PER_PAGE;

constexpr int DATADOG_WINDOWS = 50;
constexpr int DATADOG_POINTS_PER_WINDOW = 200;
constexpr int DATADOG_TOTAL = DATADOG_WINDOWS * DATADOG_POINTS_PER_WINDOW;

constexpr int OPENAI_PROMPTS = 50;

// --- Mock server handlers ---

auto generic_rest_handler(const std::string& /*method*/,
                           const std::string& path,
                           const std::string& /*body*/) -> MockServer::Response {
    int page = 0;
    auto cursor_pos = path.find("cursor=");
    if (cursor_pos != std::string::npos) {
        auto val_start = cursor_pos + 7;
        auto val_end = path.find('&', val_start);
        page = std::stoi(path.substr(val_start, val_end - val_start));
    }

    nlohmann::json j;
    j["data"] = nlohmann::json::array();
    int start = page * GENERIC_REST_RECORDS_PER_PAGE;
    for (int i = start; i < start + GENERIC_REST_RECORDS_PER_PAGE; ++i) {
        j["data"].push_back({{"id", i}});
    }
    if (page + 1 < GENERIC_REST_PAGES) {
        j["next"] = std::to_string(page + 1);
    } else {
        j["next"] = nullptr;
    }
    return MockServer::Response{200, j.dump()};
}

auto datadog_handler(const std::string& /*method*/,
                      const std::string& path,
                      const std::string& /*body*/) -> MockServer::Response {
    // Parse from/to from query params
    auto from_pos = path.find("from=");
    int64_t from_sec = 0;
    if (from_pos != std::string::npos) {
        from_sec = std::stoll(path.substr(from_pos + 5));
    }

    // Generate synthetic series data
    nlohmann::json j;
    nlohmann::json series_entry;
    series_entry["metric"] = "system.cpu.user";
    series_entry["pointlist"] = nlohmann::json::array();
    for (int i = 0; i < DATADOG_POINTS_PER_WINDOW; ++i) {
        series_entry["pointlist"].push_back(
            nlohmann::json::array({from_sec * 1000 + i, 42.0 + i}));
    }

    // Empty series after DATADOG_WINDOWS time windows (each 3600s)
    if (from_sec >= DATADOG_WINDOWS * 3600) {
        j["series"] = nlohmann::json::array();
    } else {
        j["series"] = nlohmann::json::array({series_entry});
    }

    return MockServer::Response{200, j.dump()};
}

auto openai_handler(const std::string& /*method*/,
                     const std::string& /*path*/,
                     const std::string& body) -> MockServer::Response {
    // Parse the prompt from the request body
    auto req = nlohmann::json::parse(body);
    std::string content;
    if (req.contains("messages") && !req["messages"].empty()) {
        content = req["messages"][0].value("content", "");
    }

    nlohmann::json j;
    j["choices"] = nlohmann::json::array({
        {{"index", 0}, {"message", {{"role", "assistant"}, {"content", "Response to: " + content}}}}
    });
    j["usage"] = {{"prompt_tokens", 10}, {"completion_tokens", 20}, {"total_tokens", 30}};
    return MockServer::Response{200, j.dump()};
}

// --- Tests ---

void test_generic_rest_10k() {
    MockServer server;
    server.set_handler(generic_rest_handler);
    server.start();

    std::string config = R"({"base_url": ")" + server.url() + R"(/api"})";
    auto* h = stream_create("generic_rest", config.c_str(),
                             R"({"prefetch_depth": 0})");
    assert(h != nullptr);

    char buf[BUF_SIZE];
    int32_t count = 0;
    int total = 0;

    while (stream_has_next(h) == 1) {
        int32_t rc = stream_next_batch_v1(h, buf, BUF_SIZE, &count);
        if (rc == STREAM_EXHAUSTED) break;
        assert(rc == STREAM_OK);
        total += count;
    }

    assert(total == GENERIC_REST_TOTAL);
    stream_destroy(h);
    server.stop();
    std::cout << "  PASS: generic_rest_10k (records=" << total << ")\n";
}

void test_datadog_metrics() {
    MockServer server;
    server.set_handler(datadog_handler);
    server.start();

    std::string config = R"({"base_url": ")" + server.url() + R"(/api/v1/query",)"
        R"("api_key": "test", "app_key": "test", "query": "avg:cpu{*}"})";

    auto* h = stream_create("datadog_metrics", config.c_str(),
                             R"({"prefetch_depth": 0})");
    assert(h != nullptr);

    char buf[BUF_SIZE];
    int32_t count = 0;
    int total = 0;
    int pages = 0;

    while (stream_has_next(h) == 1) {
        int32_t rc = stream_next_batch_v1(h, buf, BUF_SIZE, &count);
        if (rc == STREAM_EXHAUSTED) break;
        assert(rc == STREAM_OK);
        total += count;
        ++pages;
    }

    // The adapter fetches one extra page (empty series → exhaustion marker)
    assert(total == DATADOG_TOTAL);
    stream_destroy(h);
    server.stop();
    std::cout << "  PASS: datadog_metrics (windows=" << pages << " points=" << total << ")\n";
}

void test_openai_completions() {
    MockServer server;
    server.set_handler(openai_handler);
    server.start();

    // Build prompts array
    nlohmann::json config;
    config["base_url"] = server.url() + "/v1/chat/completions";
    config["api_key"] = "test-key";
    config["model"] = "gpt-4";
    config["prompts"] = nlohmann::json::array();
    for (int i = 0; i < OPENAI_PROMPTS; ++i) {
        config["prompts"].push_back("Prompt " + std::to_string(i));
    }
    config["max_tokens"] = 100;

    auto* h = stream_create("openai", config.dump().c_str(),
                             R"({"prefetch_depth": 0})");
    assert(h != nullptr);

    char buf[BUF_SIZE];
    int32_t count = 0;
    int total = 0;

    while (stream_has_next(h) == 1) {
        int32_t rc = stream_next_batch_v1(h, buf, BUF_SIZE, &count);
        if (rc == STREAM_EXHAUSTED) break;
        assert(rc == STREAM_OK);
        total += count;
    }

    assert(total == OPENAI_PROMPTS);
    stream_destroy(h);
    server.stop();
    std::cout << "  PASS: openai_completions (prompts=" << total << ")\n";
}

void test_unknown_adapter_returns_null() {
    auto* h = stream_create("nonexistent", R"({"base_url":"x"})", nullptr);
    assert(h == nullptr);
    std::cout << "  PASS: unknown_adapter_returns_null\n";
}

auto main() -> int {
    std::cout << "multi_adapter_test:\n";
    test_unknown_adapter_returns_null();
    test_generic_rest_10k();
    test_datadog_metrics();
    test_openai_completions();
    std::cout << "All multi-adapter tests passed.\n";
    return 0;
}
