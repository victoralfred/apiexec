#include "core/engine.hpp"
#include "core/vendor_adapter.hpp"
#include "policy/default_policy.hpp"
#include "core/itransport.hpp"

#include <cassert>
#include <iostream>
#include <queue>
#include <string>

using namespace apiexec;

static std::unique_ptr<DefaultPolicy> seq_policy() {
    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    return std::make_unique<DefaultPolicy>(cfg);
}

// ---------- Test doubles ----------

struct TestRecord {
    std::string data;
};

// Mock transport — implements ITransport with pre-programmed responses.
// No curl dependency: this test validates engine logic in isolation.
class MockTransport : public ITransport {
public:
    void push_response(Response resp) {
        responses_.push(std::move(resp));
    }

    Response execute(const Request& /*req*/, std::atomic<bool>& /*cancel*/) override {
        if (responses_.empty()) {
            return Response{0, "", {}};
        }
        auto resp = std::move(responses_.front());
        responses_.pop();
        return resp;
    }

private:
    std::queue<Response> responses_;
};

// Mock adapter — wraps simple string data.
class MockAdapter : public VendorAdapter<TestRecord> {
public:
    Request build_request(const Cursor& /*cursor*/) override {
        return Request{Request::Method::GET, "http://mock/api", "", {}};
    }

    bool parse_response(const Response& resp, TestRecord& out) override {
        if (resp.body.empty()) return false;
        out.data = resp.body;
        return true;
    }

    Cursor next_cursor(const Cursor& current, const Response& /*resp*/) override {
        Cursor next = current;
        next.exhausted = true;
        return next;
    }

    bool is_retryable(const Response& resp) override {
        return resp.is_rate_limited() || resp.is_server_error();
    }

    std::optional<int> retry_after(const Response& resp) override {
        std::string val = resp.header("retry-after");
        if (val.empty()) return std::nullopt;
        try { return std::stoi(val); } catch (...) { return std::nullopt; }
    }
};

// ---------- Tests ----------

void test_retry_on_429_then_success() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{429, "", {{"retry-after", "1"}}});
    transport->push_response(Response{429, "", {{"retry-after", "1"}}});
    transport->push_response(Response{429, "", {{"retry-after", "1"}}});
    transport->push_response(Response{200, "success_data", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(result.records.size() == 1);
    assert(result.records[0].data == "success_data");
    std::cout << "  PASS: retry_on_429_then_success\n";
}

void test_retry_honours_retry_after_header() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{429, "", {{"retry-after", "5"}}});
    transport->push_response(Response{200, "ok", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        seq_policy(),
        std::move(transport), Cursor{}
    );

    Duration observed_delay{0};
    engine.set_sleep_fn([&](Duration d) { observed_delay = d; });

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(observed_delay.count() == 5000);
    std::cout << "  PASS: retry_honours_retry_after_header\n";
}

void test_retry_on_5xx() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{503, "", {}});
    transport->push_response(Response{500, "", {}});
    transport->push_response(Response{200, "recovered", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(result.records[0].data == "recovered");
    std::cout << "  PASS: retry_on_5xx\n";
}

void test_fail_on_4xx() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{403, "forbidden", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::CLIENT);
    assert(result.records.empty());
    std::cout << "  PASS: fail_on_4xx\n";
}

void test_max_retries_exceeded() {
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < 6; ++i) {
        transport->push_response(Response{429, "", {}});
    }

    DefaultPolicy::Config cfg;
    cfg.max_retries = 5;
    cfg.prefetch_depth_val = 0;

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::RATE_LIMIT);
    std::cout << "  PASS: max_retries_exceeded\n";
}

void test_retry_counter_resets_on_success() {
    // Verifies: retry counter resets after each successful fetch.
    // With max_retries=3:
    //   Batch 1: 429, 429, 200("batch1") → 2 retries, success → counter resets
    //   Batch 2: 429, 429, 200("batch2") → 2 retries, success
    // If counter did NOT reset, batch 2 would fail after 1 retry.

    class MultiPageAdapter : public MockAdapter {
    public:
        int page = 0;
        Cursor next_cursor(const Cursor& current, const Response& /*resp*/) override {
            Cursor next = current;
            ++page;
            next.exhausted = (page >= 2);
            next.page_token = "page" + std::to_string(page);
            return next;
        }
    };

    auto transport = std::make_unique<MockTransport>();
    // Batch 1
    transport->push_response(Response{429, "", {}});
    transport->push_response(Response{429, "", {}});
    transport->push_response(Response{200, "batch1", {}});
    // Batch 2
    transport->push_response(Response{429, "", {}});
    transport->push_response(Response{429, "", {}});
    transport->push_response(Response{200, "batch2", {}});

    DefaultPolicy::Config cfg;
    cfg.max_retries = 3;
    cfg.prefetch_depth_val = 0;

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MultiPageAdapter>(),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result1 = engine.next_batch();
    assert(result1.error == StreamErrorCode::OK);
    assert(result1.records[0].data == "batch1");

    assert(engine.has_next());
    auto result2 = engine.next_batch();
    assert(result2.error == StreamErrorCode::OK);
    assert(result2.records[0].data == "batch2");

    assert(!engine.has_next());
    std::cout << "  PASS: retry_counter_resets_on_success\n";
}

void test_network_error_retry() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{0, "", {}});
    transport->push_response(Response{0, "", {}});
    transport->push_response(Response{200, "recovered_from_network", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(result.records[0].data == "recovered_from_network");
    std::cout << "  PASS: network_error_retry\n";
}

void test_parse_error_fails_immediately() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{200, "", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::PARSE);
    std::cout << "  PASS: parse_error_fails_immediately\n";
}

void test_403_does_not_shrink_window() {
    // A 403 is a client error, NOT a load signal. Window must remain unchanged.
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{403, "forbidden", {}});

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;

    Cursor cursor;
    cursor.time_window_start = 0;
    cursor.time_window_end = 3'600'000;  // 1h
    int64_t original_window = cursor.time_window_end;

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), cursor
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::CLIENT);
    // Window must NOT have changed
    assert(engine.cursor().time_window_end == original_window);
    std::cout << "  PASS: 403_does_not_shrink_window\n";
}

void test_429_shrinks_window_on_first_encounter() {
    // 429 → 429 → 200. Window should shrink once (first 429), not grow on success.
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{429, "", {}});
    transport->push_response(Response{429, "", {}});
    transport->push_response(Response{200, "ok", {}});

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    cfg.window_shrink_factor = 0.5;

    Cursor cursor;
    cursor.time_window_start = 0;
    cursor.time_window_end = 3'600'000;  // 1h
    int64_t expected_after_shrink = 1'800'000;  // 0.5x

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), cursor
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    // Window should have shrunk from the 429 and NOT grown back on the 200
    assert(engine.cursor().time_window_end == expected_after_shrink);
    std::cout << "  PASS: 429_shrinks_window_on_first_encounter\n";
}

void test_5xx_terminal_does_not_shrink_window() {
    // 6 consecutive 503s (exceeds max_retries=5). Window must remain unchanged.
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < 6; ++i) {
        transport->push_response(Response{503, "", {}});
    }

    DefaultPolicy::Config cfg;
    cfg.max_retries = 5;
    cfg.prefetch_depth_val = 0;

    Cursor cursor;
    cursor.time_window_start = 0;
    cursor.time_window_end = 3'600'000;
    int64_t original_window = cursor.time_window_end;

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), cursor
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::SERVER);
    // Window must NOT have changed — server errors are not load signals
    assert(engine.cursor().time_window_end == original_window);
    std::cout << "  PASS: 5xx_terminal_does_not_shrink_window\n";
}

void test_success_without_429_grows_window() {
    // Clean success (no 429s) should grow the window.
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{200, "ok", {}});

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    cfg.window_grow_factor = 1.5;

    Cursor cursor;
    cursor.time_window_start = 0;
    cursor.time_window_end = 3'600'000;  // 1h
    int64_t expected = 5'400'000;  // 1.5x

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MockAdapter>(),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), cursor
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(engine.cursor().time_window_end == expected);
    std::cout << "  PASS: success_without_429_grows_window\n";
}

auto main() -> int {
    std::cout << "retry_test:\n";
    test_retry_on_429_then_success();
    test_retry_honours_retry_after_header();
    test_retry_on_5xx();
    test_fail_on_4xx();
    test_max_retries_exceeded();
    test_retry_counter_resets_on_success();
    test_network_error_retry();
    test_parse_error_fails_immediately();
    test_403_does_not_shrink_window();
    test_429_shrinks_window_on_first_encounter();
    test_5xx_terminal_does_not_shrink_window();
    test_success_without_429_grows_window();
    std::cout << "All retry tests passed.\n";
    return 0;
}
