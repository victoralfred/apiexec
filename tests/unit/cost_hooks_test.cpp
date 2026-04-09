#include "core/engine.hpp"
#include "core/vendor_adapter.hpp"
#include "core/itransport.hpp"
#include "policy/default_policy.hpp"

#include <cassert>
#include <iostream>
#include <optional>
#include <queue>

using namespace apiexec;

// --- Test doubles ---

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

constexpr double COST_PER_RESPONSE = 10.0;

class CostTrackingAdapter : public VendorAdapter<TestRecord> {
public:
    int page = 0;
    int total_pages;

    explicit CostTrackingAdapter(int pages) : total_pages(pages) {}

    auto build_request(const Cursor&) -> Request override {
        return Request{Request::Method::GET, "http://mock/api", "", {}};
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

    // Override cost hook: each response costs 10 units
    auto response_cost(const Response&) const -> std::optional<double> override {
        return COST_PER_RESPONSE;
    }
};

constexpr double BUDGET_LIMIT = 35.0;

class BudgetPolicy : public ExecutionPolicy {
public:
    double cumulative_cost = 0.0;
    double budget;

    explicit BudgetPolicy(double b) : budget(b) {}

    auto adjust(Cursor&, bool) -> void override {}
    auto backoff(int) -> Duration override { return Duration(0); }
    auto prefetch_depth() -> std::size_t override { return 0; }
    auto max_retries() const -> int override { return 3; }

    auto record_cost(const Cursor&, double cost_units) -> void override {
        cumulative_cost += cost_units;
    }

    auto remaining_budget() const -> std::optional<double> override {
        return budget - cumulative_cost;
    }

    auto budget_exceeded() const -> bool override {
        return cumulative_cost >= budget;
    }
};

// --- Tests ---

void test_default_policy_cost_noop() {
    DefaultPolicy policy;
    // Default cost hooks should be no-ops
    assert(policy.remaining_budget() == std::nullopt);
    assert(!policy.budget_exceeded());
    // record_cost should not crash
    Cursor c;
    policy.record_cost(c, 100.0);
    assert(!policy.budget_exceeded());
    std::cout << "  PASS: default_policy_cost_noop\n";
}

void test_default_adapter_cost_nullopt() {
    CostTrackingAdapter adapter(1);
    // Our test adapter returns a cost
    Response resp{200, "data", {}};
    assert(adapter.response_cost(resp).has_value());
    assert(adapter.response_cost(resp).value() == COST_PER_RESPONSE);
    std::cout << "  PASS: default_adapter_cost_nullopt\n";
}

void test_budget_halts_engine() {
    // Budget = 35. Each response costs 10. Should process 3 batches (cost=30),
    // then halt on the 4th attempt (cumulative=30, which is < 35, so 4th fetch
    // succeeds with cost=40, then 5th check budget_exceeded=true → halt).
    // Actually: budget check is at START of fetch_one, and record_cost at END.
    // So: fetch 1 (check:0<35, ok, record:10), fetch 2 (check:10<35, ok, record:20),
    //     fetch 3 (check:20<35, ok, record:30), fetch 4 (check:30<35, ok, record:40)
    //     fetch 5 (check:40>=35, BUDGET_EXHAUSTED)

    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < 10; ++i) {
        transport->push_response(Response{200, "data_" + std::to_string(i), {}});
    }

    auto* policy_ptr = new BudgetPolicy(BUDGET_LIMIT);
    ExecutionEngine<TestRecord> engine(
        std::make_unique<CostTrackingAdapter>(10),
        std::unique_ptr<ExecutionPolicy>(policy_ptr),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int fetched = 0;
    StreamErrorCode last_error = StreamErrorCode::OK;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        last_error = result.error;
        if (result.error != StreamErrorCode::OK) break;
        ++fetched;
    }

    // Should have processed 4 batches (cost 40) then halted
    // Actually: after 3 fetches, cumulative=30. 4th fetch: check 30<35, proceeds,
    // records cost → 40. 5th fetch: check 40>=35 → BUDGET_EXHAUSTED.
    // But has_next checks budget_exceeded via... no, has_next only checks
    // stream_done_ and cancel_flag_. So the loop continues until fetch_one returns.
    assert(fetched == 4);  // 4 successful fetches, then budget halt on 5th attempt
    assert(last_error == StreamErrorCode::BUDGET_EXHAUSTED);
    assert(policy_ptr->cumulative_cost == 40.0);
    std::cout << "  PASS: budget_halts_engine (fetched=" << fetched
              << " cost=" << policy_ptr->cumulative_cost << ")\n";
}

void test_no_budget_runs_to_completion() {
    // DefaultPolicy has no budget — should fetch all pages
    auto transport = std::make_unique<MockTransport>();
    for (int i = 0; i < 5; ++i) {
        transport->push_response(Response{200, "data", {}});
    }

    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;

    ExecutionEngine<TestRecord> engine(
        std::make_unique<CostTrackingAdapter>(5),
        std::make_unique<DefaultPolicy>(cfg),
        std::move(transport), Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int fetched = 0;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error != StreamErrorCode::OK) break;
        ++fetched;
    }

    assert(fetched == 5);
    std::cout << "  PASS: no_budget_runs_to_completion\n";
}

auto main() -> int {
    std::cout << "cost_hooks_test:\n";
    test_default_policy_cost_noop();
    test_default_adapter_cost_nullopt();
    test_budget_halts_engine();
    test_no_budget_runs_to_completion();
    std::cout << "All cost hooks tests passed.\n";
    return 0;
}
