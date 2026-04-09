#include "core/engine.hpp"
#include "adapters/generic_rest.hpp"
#include "policy/default_policy.hpp"
#include "transport/curl_transport.hpp"
#include "mock_server/mock_server.hpp"

#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace apiexec;
using namespace apiexec::test;

MockServer::Response page_handler(const std::string& /*method*/,
                                   const std::string& path,
                                   const std::string& /*body*/) {
    int page = 0;
    auto cursor_pos = path.find("cursor=");
    if (cursor_pos != std::string::npos) {
        auto val_start = cursor_pos + 7;
        auto val_end = path.find('&', val_start);
        std::string token = path.substr(val_start, val_end - val_start);
        page = std::stoi(token);
    }

    const int total_pages = 5;
    const int records_per_page = 10;

    nlohmann::json j;
    j["data"] = nlohmann::json::array();
    int start = page * records_per_page;
    for (int i = start; i < start + records_per_page; ++i) {
        j["data"].push_back({{"id", i}, {"value", "record_" + std::to_string(i)}});
    }

    if (page + 1 < total_pages) {
        j["next"] = std::to_string(page + 1);
    } else {
        j["next"] = nullptr;
    }

    return MockServer::Response{200, j.dump()};
}

void test_e2e_paginated_fetch() {
    MockServer server;
    server.set_handler(page_handler);
    server.start();

    GenericRestAdapter::Config cfg;
    cfg.base_url = server.url() + "/api/data";
    cfg.page_size = 10;
    cfg.data_field = "data";
    cfg.next_token_field = "next";
    cfg.page_param = "cursor";

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(cfg),
        std::make_unique<DefaultPolicy>(),
        std::make_unique<CurlTransport>(),
        Cursor{}
    );

    int total_records = 0;
    int pages_fetched = 0;

    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error == StreamErrorCode::EXHAUSTED) break;
        assert(result.error == StreamErrorCode::OK);
        for (const auto& batch : result.records) {
            total_records += batch.count;
        }
        ++pages_fetched;
    }

    assert(pages_fetched == 5);
    assert(total_records == 50);
    assert(!engine.has_next());

    server.stop();
    std::cout << "  PASS: e2e_paginated_fetch (pages=" << pages_fetched
              << " records=" << total_records << ")\n";
}

void test_e2e_retry_on_429() {
    int request_count = 0;

    MockServer server;
    server.set_handler([&](const std::string& /*method*/,
                           const std::string& /*path*/,
                           const std::string& /*body*/) -> MockServer::Response {
        ++request_count;
        if (request_count <= 2) {
            return MockServer::Response{
                429, R"({"error":"rate limited"})", "application/json",
                "Retry-After: 0\r\n"
            };
        }

        nlohmann::json j;
        j["data"] = {{{"id", 1}, {"value", "success"}}};
        j["next"] = nullptr;
        return MockServer::Response{200, j.dump()};
    });
    server.start();

    GenericRestAdapter::Config cfg;
    cfg.base_url = server.url() + "/api/data";
    cfg.page_size = 10;

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(cfg),
        std::make_unique<DefaultPolicy>(),
        std::make_unique<CurlTransport>(),
        Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(result.records.size() == 1);
    assert(result.records[0].count == 1);
    assert(request_count == 3);

    server.stop();
    std::cout << "  PASS: e2e_retry_on_429 (requests=" << request_count << ")\n";
}

void test_e2e_fail_on_4xx() {
    MockServer server;
    server.set_handler([](const std::string& /*method*/,
                          const std::string& /*path*/,
                          const std::string& /*body*/) -> MockServer::Response {
        return MockServer::Response{403, R"({"error":"forbidden"})"};
    });
    server.start();

    GenericRestAdapter::Config cfg;
    cfg.base_url = server.url() + "/api/data";

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(cfg),
        std::make_unique<DefaultPolicy>(),
        std::make_unique<CurlTransport>(),
        Cursor{}
    );

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::CLIENT);

    server.stop();
    std::cout << "  PASS: e2e_fail_on_4xx\n";
}

int main() {
    std::cout << "e2e_test (integration):\n";
    test_e2e_paginated_fetch();
    test_e2e_retry_on_429();
    test_e2e_fail_on_4xx();
    std::cout << "All E2E integration tests passed.\n";
    return 0;
}
