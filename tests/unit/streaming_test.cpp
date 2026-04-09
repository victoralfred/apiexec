#include "core/engine.hpp"
#include "core/vendor_adapter.hpp"
#include "core/itransport.hpp"
#include "policy/default_policy.hpp"

#include <cassert>
#include <iostream>
#include <queue>

using namespace apiexec;

struct TestRecord {
    std::string data;
};

class MockTransport : public ITransport {
public:
    auto push_response(Response resp) -> void {
        responses_.push(std::move(resp));
    }

    auto execute(const Request&, std::atomic<bool>&) -> Response override {
        if (responses_.empty()) return Response{0, "", {}};
        auto resp = std::move(responses_.front());
        responses_.pop();
        return resp;
    }

private:
    std::queue<Response> responses_;
};

class SimpleAdapter : public VendorAdapter<TestRecord> {
public:
    int page = 0;
    int total_pages;

    explicit SimpleAdapter(int pages) : total_pages(pages) {}

    auto build_request(const Cursor&) -> Request override {
        return Request{Request::Method::GET, "http://mock", "", {}};
    }

    auto parse_response(const Response& resp, TestRecord& out) -> bool override {
        out.data = resp.body;
        return !resp.body.empty();
    }

    auto next_cursor(const Cursor& current, const Response&) -> Cursor override {
        Cursor next = current;
        ++page;
        next.exhausted = (page >= total_pages);
        next.page_token = "p" + std::to_string(page);
        return next;
    }

    auto is_retryable(const Response& resp) -> bool override {
        return resp.is_rate_limited() || resp.is_server_error();
    }

    auto retry_after(const Response&) -> std::optional<int> override {
        return std::nullopt;
    }
};

static auto seq_policy() -> std::unique_ptr<DefaultPolicy> {
    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    return std::make_unique<DefaultPolicy>(cfg);
}

void test_stream_callback_receives_all_records() {
    constexpr int num_pages = 5;
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "record_" + std::to_string(i), {}});
    }

    ExecutionEngine<TestRecord> engine(
        std::make_unique<SimpleAdapter>(num_pages),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int count = 0;
    auto err = engine.stream([&](const TestRecord& rec) -> bool {
        assert(rec.data == "record_" + std::to_string(count));
        ++count;
        return true;
    });

    assert(err == StreamErrorCode::OK);
    assert(count == num_pages);
    std::cout << "  PASS: stream_callback_receives_all_records (count=" << count << ")\n";
}

void test_stream_callback_stop_early() {
    constexpr int num_pages = 10;
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "data", {}});
    }

    ExecutionEngine<TestRecord> engine(
        std::make_unique<SimpleAdapter>(num_pages),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int count = 0;
    auto err = engine.stream([&](const TestRecord&) -> bool {
        ++count;
        return count < 3;  // stop after 3
    });

    assert(err == StreamErrorCode::CANCELLED);
    assert(count == 3);
    std::cout << "  PASS: stream_callback_stop_early (stopped at " << count << ")\n";
}

void test_stream_propagates_error() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{403, "forbidden", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<SimpleAdapter>(5),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int count = 0;
    auto err = engine.stream([&](const TestRecord&) -> bool {
        ++count;
        return true;
    });

    assert(err == StreamErrorCode::CLIENT);
    assert(count == 0);
    std::cout << "  PASS: stream_propagates_error\n";
}

void test_supports_streaming_default_false() {
    SimpleAdapter adapter(1);
    assert(!adapter.supports_streaming());
    std::cout << "  PASS: supports_streaming_default_false\n";
}

auto main() -> int {
    std::cout << "streaming_test:\n";
    test_stream_callback_receives_all_records();
    test_stream_callback_stop_early();
    test_stream_propagates_error();
    test_supports_streaming_default_false();
    std::cout << "All streaming tests passed.\n";
    return 0;
}
