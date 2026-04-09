#include "core/engine.hpp"
#include "core/vendor_adapter.hpp"
#include "core/itransport.hpp"
#include "policy/default_policy.hpp"

#include <cassert>
#include <iostream>
#include <queue>

using namespace apiexec;

struct TestRecord { std::string data; };

class MockTransport : public ITransport {
public:
    auto push(Response r) -> void { q_.push(std::move(r)); }
    auto execute(const Request&, std::atomic<bool>&) -> Response override {
        if (q_.empty()) return Response{0, "", {}};
        auto r = std::move(q_.front()); q_.pop(); return r;
    }
private:
    std::queue<Response> q_;
};

class SimpleAdapter : public VendorAdapter<TestRecord> {
public:
    int page = 0, total;
    explicit SimpleAdapter(int t) : total(t) {}
    auto build_request(const Cursor&) -> Request override { return {}; }
    auto parse_response(const Response& r, TestRecord& out) -> bool override {
        out.data = r.body; return !r.body.empty();
    }
    auto next_cursor(const Cursor& c, const Response&) -> Cursor override {
        Cursor n = c; ++page; n.exhausted = (page >= total);
        n.page_token = std::to_string(page); return n;
    }
    auto is_retryable(const Response& r) -> bool override {
        return r.is_rate_limited() || r.is_server_error();
    }
    auto retry_after(const Response&) -> std::optional<int> override { return std::nullopt; }
};

static auto seq_policy() -> std::unique_ptr<DefaultPolicy> {
    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    return std::make_unique<DefaultPolicy>(cfg);
}

void test_metrics_increment_on_success() {
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < 3; ++i) {
        transport->push(Response{200, "data_" + std::to_string(i), {}});
    }

    ExecutionEngine<TestRecord> engine(
        std::make_unique<SimpleAdapter>(3), seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    while (engine.has_next()) {
        auto r = engine.next_batch();
        if (r.error != StreamErrorCode::OK) break;
    }

    auto s = engine.metrics_snapshot();
    assert(s.request_count == 3);
    assert(s.success_count == 3);
    assert(s.records_total == 3);
    assert(s.retry_count == 0);
    assert(s.error_rate_limit == 0);
    std::cout << "  PASS: metrics_increment_on_success\n";
}

void test_metrics_increment_on_retry() {
    auto transport = std::make_unique<MockTransport>();
    transport->push(Response{429, "", {}});
    transport->push(Response{429, "", {}});
    transport->push(Response{200, "ok", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<SimpleAdapter>(1), seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    engine.next_batch();
    auto s = engine.metrics_snapshot();
    assert(s.request_count == 3);   // 2 retries + 1 success
    assert(s.retry_count == 2);
    assert(s.success_count == 1);
    std::cout << "  PASS: metrics_increment_on_retry\n";
}

void test_metrics_increment_on_terminal_error() {
    auto transport = std::make_unique<MockTransport>();
    transport->push(Response{403, "forbidden", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<SimpleAdapter>(1), seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    engine.next_batch();
    auto s = engine.metrics_snapshot();
    assert(s.request_count == 1);
    assert(s.error_client == 1);
    assert(s.success_count == 0);
    std::cout << "  PASS: metrics_increment_on_terminal_error\n";
}

void test_metrics_callback_fires() {
    auto transport = std::make_unique<MockTransport>();
    transport->push(Response{200, "data", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<SimpleAdapter>(1), seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int cb_count = 0;
    engine.set_metrics_callback([&](const MetricsSnapshot& s) {
        ++cb_count;
        assert(s.success_count == 1);
        assert(s.records_total == 1);
    });

    engine.next_batch();
    assert(cb_count == 1);
    std::cout << "  PASS: metrics_callback_fires\n";
}

void test_prometheus_format() {
    Metrics m;
    m.inc_request();
    m.inc_request();
    m.inc_success();
    m.inc_retry();
    m.set_window_size_ms(3600000.0);

    auto prom = m.to_prometheus("myapp");
    assert(prom.find("myapp_requests_total 2") != std::string::npos);
    assert(prom.find("myapp_successes_total 1") != std::string::npos);
    assert(prom.find("myapp_retries_total 1") != std::string::npos);
    assert(prom.find("myapp_window_size_ms") != std::string::npos);
    std::cout << "  PASS: prometheus_format\n";
}

auto main() -> int {
    std::cout << "metrics_test:\n";
    test_metrics_increment_on_success();
    test_metrics_increment_on_retry();
    test_metrics_increment_on_terminal_error();
    test_metrics_callback_fires();
    test_prometheus_format();
    std::cout << "All metrics tests passed.\n";
    return 0;
}
