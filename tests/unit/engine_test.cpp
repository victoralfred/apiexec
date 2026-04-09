#include "core/engine.hpp"
#include "adapters/generic_rest.hpp"
#include "policy/default_policy.hpp"
#include "core/itransport.hpp"

#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>
#include <queue>

using namespace apiexec;

static std::unique_ptr<DefaultPolicy> seq_policy() {
    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    return std::make_unique<DefaultPolicy>(cfg);
}

class MockTransport : public ITransport {
public:
    void push_response(Response resp) {
        responses_.push(std::move(resp));
    }

    int request_count() const { return request_count_; }

    Response execute(const Request& req, std::atomic<bool>& /*cancel*/) override {
        ++request_count_;
        last_url_ = req.url;
        if (responses_.empty()) {
            return Response{0, "", {}};
        }
        auto resp = std::move(responses_.front());
        responses_.pop();
        return resp;
    }

    const std::string& last_url() const { return last_url_; }

private:
    std::queue<Response> responses_;
    int request_count_ = 0;
    std::string last_url_;
};

std::string make_page(int start, int count, const std::string& next_token = "") {
    nlohmann::json j;
    j["data"] = nlohmann::json::array();
    for (int i = start; i < start + count; ++i) {
        j["data"].push_back({{"id", i}, {"value", "record_" + std::to_string(i)}});
    }
    if (!next_token.empty()) {
        j["next"] = next_token;
    } else {
        j["next"] = nullptr;
    }
    return j.dump();
}

void test_generic_rest_single_page() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{200, make_page(0, 3), {}});

    GenericRestAdapter::Config cfg;
    cfg.base_url = "http://mock/api";
    cfg.page_size = 10;

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(cfg),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(result.records.size() == 1);
    assert(result.records[0].count == 3);
    assert(!engine.has_next());
    std::cout << "  PASS: generic_rest_single_page\n";
}

void test_generic_rest_multi_page() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{200, make_page(0, 5, "token_1"), {}});
    transport->push_response(Response{200, make_page(5, 5, "token_2"), {}});
    transport->push_response(Response{200, make_page(10, 3), {}});

    GenericRestAdapter::Config cfg;
    cfg.base_url = "http://mock/api";
    cfg.page_size = 5;

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(cfg),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int total_records = 0;
    int pages = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error != StreamErrorCode::OK) break;
        for (const auto& batch : result.records) {
            total_records += batch.count;
        }
        ++pages;
    }

    assert(pages == 3);
    assert(total_records == 13);
    assert(!engine.has_next());
    std::cout << "  PASS: generic_rest_multi_page (pages=" << pages << " records=" << total_records << ")\n";
}

void test_generic_rest_config_from_json() {
    std::string json = R"({
        "base_url": "https://api.example.com/v1",
        "auth_header": "Bearer test123",
        "data_field": "results",
        "next_token_field": "pagination.next",
        "page_param": "after",
        "page_size": 50
    })";

    auto cfg = GenericRestAdapter::from_json(json);
    assert(cfg.base_url == "https://api.example.com/v1");
    assert(cfg.auth_header == "Bearer test123");
    assert(cfg.data_field == "results");
    assert(cfg.page_param == "after");
    assert(cfg.page_size == 50);
    std::cout << "  PASS: generic_rest_config_from_json\n";
}

void test_generic_rest_url_construction() {
    auto transport_ptr = std::make_unique<MockTransport>();
    auto* transport_raw = transport_ptr.get();
    transport_ptr->push_response(Response{200, make_page(0, 1), {}});

    GenericRestAdapter::Config cfg;
    cfg.base_url = "http://mock/api";
    cfg.page_param = "cursor";
    cfg.page_size = 25;

    Cursor cursor;
    cursor.page_token = "abc123";

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(cfg),
        seq_policy(),
        std::move(transport_ptr), cursor
    );
    engine.set_sleep_fn([](Duration) {});

    engine.next_batch();
    assert(transport_raw->last_url().find("cursor=abc123") != std::string::npos);
    assert(transport_raw->last_url().find("limit=25") != std::string::npos);
    std::cout << "  PASS: generic_rest_url_construction\n";
}

int main() {
    std::cout << "engine_test:\n";
    test_generic_rest_single_page();
    test_generic_rest_multi_page();
    test_generic_rest_config_from_json();
    test_generic_rest_url_construction();
    std::cout << "All engine tests passed.\n";
    return 0;
}
