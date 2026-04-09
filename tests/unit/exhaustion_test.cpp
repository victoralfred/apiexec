#include "core/engine.hpp"
#include "core/vendor_adapter.hpp"
#include "policy/default_policy.hpp"
#include "core/itransport.hpp"

#include <cassert>
#include <iostream>
#include <queue>

using namespace apiexec;

static std::unique_ptr<DefaultPolicy> seq_policy() {
    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    return std::make_unique<DefaultPolicy>(cfg);
}

struct TestRecord {
    std::string data;
};

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

class ExhaustingAdapter : public VendorAdapter<TestRecord> {
public:
    int total_pages;
    int current_page = 0;

    explicit ExhaustingAdapter(int pages) : total_pages(pages) {}

    Request build_request(const Cursor& /*cursor*/) override {
        return Request{Request::Method::GET, "http://mock/api", "", {}};
    }

    bool parse_response(const Response& resp, TestRecord& out) override {
        out.data = resp.body;
        return !resp.body.empty();
    }

    Cursor next_cursor(const Cursor& current, const Response& /*resp*/) override {
        Cursor next = current;
        ++current_page;
        if (current_page >= total_pages) {
            next.exhausted = true;
        } else {
            next.page_token = "page_" + std::to_string(current_page);
        }
        return next;
    }

    bool is_retryable(const Response& resp) override {
        return resp.is_rate_limited() || resp.is_server_error();
    }

    std::optional<int> retry_after(const Response& /*resp*/) override {
        return std::nullopt;
    }
};

void test_engine_terminates_on_exhaustion() {
    const int num_pages = 3;
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "page_" + std::to_string(i), {}});
    }

    ExecutionEngine<TestRecord> engine(
        std::make_unique<ExhaustingAdapter>(num_pages),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int fetched = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error == StreamErrorCode::EXHAUSTED) break;
        assert(result.error == StreamErrorCode::OK);
        ++fetched;
    }

    assert(fetched == num_pages);
    assert(!engine.has_next());
    assert(engine.cursor().exhausted);
    std::cout << "  PASS: engine_terminates_on_exhaustion (fetched " << fetched << " pages)\n";
}

void test_immediate_exhaustion() {
    auto transport = std::make_unique<MockTransport>();
    transport->push_response(Response{200, "only_page", {}});

    ExecutionEngine<TestRecord> engine(
        std::make_unique<ExhaustingAdapter>(1),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);
    assert(result.records[0].data == "only_page");
    assert(!engine.has_next());

    auto result2 = engine.next_batch();
    assert(result2.error == StreamErrorCode::EXHAUSTED);
    std::cout << "  PASS: immediate_exhaustion\n";
}

void test_no_infinite_loop() {
    const int num_pages = 100;
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "data", {}});
    }

    ExecutionEngine<TestRecord> engine(
        std::make_unique<ExhaustingAdapter>(num_pages),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int fetched = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error != StreamErrorCode::OK) break;
        ++fetched;
        assert(fetched <= num_pages + 1);
    }

    assert(fetched == num_pages);
    std::cout << "  PASS: no_infinite_loop (fetched " << fetched << " pages)\n";
}

void test_cancellation() {
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < 10; ++i) {
        transport->push_response(Response{200, "data", {}});
    }

    ExecutionEngine<TestRecord> engine(
        std::make_unique<ExhaustingAdapter>(10),
        seq_policy(),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    auto result = engine.next_batch();
    assert(result.error == StreamErrorCode::OK);

    engine.cancel();
    assert(!engine.has_next());

    auto result2 = engine.next_batch();
    assert(result2.error == StreamErrorCode::CANCELLED);
    std::cout << "  PASS: cancellation\n";
}

int main() {
    std::cout << "exhaustion_test:\n";
    test_engine_terminates_on_exhaustion();
    test_immediate_exhaustion();
    test_no_infinite_loop();
    test_cancellation();
    std::cout << "All exhaustion tests passed.\n";
    return 0;
}
