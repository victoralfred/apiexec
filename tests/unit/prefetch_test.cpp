#include "core/engine.hpp"
#include "core/vendor_adapter.hpp"
#include "core/itransport.hpp"
#include "policy/default_policy.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

using namespace apiexec;

struct TestRecord {
    std::string data;
};

// Thread-safe mock transport with configurable latency.
class MockTransport : public ITransport {
public:
    void push_response(Response resp) {
        std::lock_guard<std::mutex> lock(mu_);
        responses_.push(std::move(resp));
    }

    void set_latency(Duration d) { latency_ = d; }

    int request_count() const { return request_count_.load(); }

    Response execute(const Request& /*req*/, std::atomic<bool>& cancel) override {
        request_count_.fetch_add(1);
        // Simulate network latency
        if (latency_.count() > 0) {
            std::this_thread::sleep_for(latency_);
        }
        if (cancel.load()) return Response{0, "", {}};
        std::lock_guard<std::mutex> lock(mu_);
        if (responses_.empty()) return Response{0, "", {}};
        auto resp = std::move(responses_.front());
        responses_.pop();
        return resp;
    }

private:
    std::mutex mu_;
    std::queue<Response> responses_;
    Duration latency_{0};
    std::atomic<int> request_count_{0};
};

class MultiPageAdapter : public VendorAdapter<TestRecord> {
public:
    int total_pages;
    std::atomic<int> current_page{0};

    explicit MultiPageAdapter(int pages) : total_pages(pages) {}

    Request build_request(const Cursor& /*cursor*/) override {
        return Request{Request::Method::GET, "http://mock/api", "", {}};
    }

    bool parse_response(const Response& resp, TestRecord& out) override {
        out.data = resp.body;
        return !resp.body.empty();
    }

    Cursor next_cursor(const Cursor& current, const Response& /*resp*/) override {
        Cursor next = current;
        int page = current_page.fetch_add(1) + 1;
        if (page >= total_pages) {
            next.exhausted = true;
        } else {
            next.page_token = "page_" + std::to_string(page);
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

void test_prefetch_basic() {
    const int num_pages = 5;
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "page_" + std::to_string(i), {}});
    }

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 1;  // enable double-buffer

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MultiPageAdapter>(num_pages),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), Cursor{}
    );

    int fetched = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error == StreamErrorCode::EXHAUSTED) break;
        assert(result.error == StreamErrorCode::OK);
        assert(result.records[0].data == "page_" + std::to_string(fetched));
        ++fetched;
    }

    assert(fetched == num_pages);
    assert(!engine.has_next());
    std::cout << "  PASS: prefetch_basic (fetched " << fetched << " pages)\n";
}

void test_prefetch_overlaps_io() {
    // Verify prefetch actually overlaps I/O with processing.
    // Each fetch takes 20ms. Processing takes 20ms.
    // Sequential: 10 pages * (20ms fetch + 20ms process) = 400ms
    // Prefetch:   20ms first fetch + 10 * 20ms process ≈ 220ms (first fetch serial, rest overlap)
    // We assert wall time < 350ms to confirm overlap (generous margin).

    const int num_pages = 10;
    auto transport = std::make_unique<MockTransport>();
    transport->set_latency(Duration(20));
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "data_" + std::to_string(i), {}});
    }

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 1;

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MultiPageAdapter>(num_pages),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), Cursor{}
    );

    auto start = std::chrono::steady_clock::now();
    int fetched = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error != StreamErrorCode::OK) break;
        // Simulate 20ms processing
        std::this_thread::sleep_for(Duration(20));
        ++fetched;
    }
    auto elapsed = std::chrono::duration_cast<Duration>(
        std::chrono::steady_clock::now() - start);

    assert(fetched == num_pages);
    // Sequential would be ≥ 400ms. Prefetch should be < 350ms.
    assert(elapsed.count() < 350);
    std::cout << "  PASS: prefetch_overlaps_io (fetched " << fetched
              << " pages in " << elapsed.count() << "ms)\n";
}

void test_prefetch_vs_sequential_timing() {
    // Direct timing comparison: sequential vs prefetch with identical workload.
    const int num_pages = 10;
    const int fetch_ms = 15;
    const int process_ms = 15;

    // Sequential run (prefetch_depth = 0)
    auto run = [&](std::size_t depth) -> int64_t {
        auto transport = std::make_unique<MockTransport>();
        transport->set_latency(Duration(fetch_ms));
        for (int i = 0; i < num_pages; ++i) {
            transport->push_response(Response{200, "data", {}});
        }
        DefaultPolicy::Config cfg;
        cfg.prefetch_depth_val = depth;

        ExecutionEngine<TestRecord> engine(
            std::make_unique<MultiPageAdapter>(num_pages),
            std::make_unique<DefaultPolicy>(cfg),
            std::move(transport), Cursor{}
        );

        auto start = std::chrono::steady_clock::now();
        while (engine.has_next()) {
            auto result = engine.next_batch();
            if (result.error != StreamErrorCode::OK) break;
            std::this_thread::sleep_for(Duration(process_ms));
        }
        return std::chrono::duration_cast<Duration>(
            std::chrono::steady_clock::now() - start).count();
    };

    int64_t seq_ms = run(0);
    int64_t pre_ms = run(1);
    double improvement = 100.0 * (seq_ms - pre_ms) / seq_ms;

    std::cout << "  sequential=" << seq_ms << "ms  prefetch=" << pre_ms
              << "ms  improvement=" << improvement << "%\n";
    assert(improvement >= 30.0);  // At least 30% (conservative for CI jitter)
    std::cout << "  PASS: prefetch_vs_sequential_timing\n";
}

void test_prefetch_cancellation() {
    const int num_pages = 100;
    auto transport = std::make_unique<MockTransport>();
    transport->set_latency(Duration(5));
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "data", {}});
    }

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 1;

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MultiPageAdapter>(num_pages),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), Cursor{}
    );

    // Fetch a couple pages then cancel
    engine.next_batch();
    engine.next_batch();
    engine.cancel();

    // May still have an in-flight prefetch to drain
    while (engine.has_next()) {
        auto result = engine.next_batch();
        // Should get OK (from already-completed prefetch) or CANCELLED
        assert(result.error == StreamErrorCode::OK ||
               result.error == StreamErrorCode::CANCELLED ||
               result.error == StreamErrorCode::EXHAUSTED);
    }

    assert(!engine.has_next());
    std::cout << "  PASS: prefetch_cancellation\n";
}

void test_sequential_mode_unchanged() {
    // Verify prefetch_depth=0 still works (M1 behaviour preserved)
    const int num_pages = 3;
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < num_pages; ++i) {
        transport->push_response(Response{200, "page_" + std::to_string(i), {}});
    }

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;  // sequential

    ExecutionEngine<TestRecord> engine(
        std::make_unique<MultiPageAdapter>(num_pages),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), Cursor{}
    );

    int fetched = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error != StreamErrorCode::OK) break;
        ++fetched;
    }
    assert(fetched == num_pages);
    std::cout << "  PASS: sequential_mode_unchanged\n";
}

int main() {
    std::cout << "prefetch_test:\n";
    test_prefetch_basic();
    test_prefetch_overlaps_io();
    test_prefetch_vs_sequential_timing();
    test_prefetch_cancellation();
    test_sequential_mode_unchanged();
    std::cout << "All prefetch tests passed.\n";
    return 0;
}
